/**
 * ============================================================
 * mono_runtime.cpp — Mono 高层运行时实现
 * ============================================================
 * 负责把回调式 Mono Assembly 枚举转换为稳定的 C++ 快照，并向 Binding
 * 提供大小写无关、可省略 .dll 的查找语义。快照变化同步 metadata
 * generation；对象创建和静态字段通过调用线程当前 Domain 完成。
 * ============================================================
 */
#include "mono_runtime.h"
#include "mono_hook.h"
#include "mono_resolver.h"
#include "mono_scheduler.h"

#include <algorithm>
#include <cctype>
#include <sstream>

MonoRuntime& MonoRuntime::Instance()
{
    static MonoRuntime runtime;
    return runtime;
}

MonoDomain* MonoRuntime::Domain() const
{
    MonoDomain* current = MonoResolver::Instance().CurrentDomain();
    return current ? current : m_domain;
}

bool MonoRuntime::Init()
{
    if (m_initialized) return true;
    m_lastError.clear();
    m_lastStage = "resolving Mono module, exports, and Root Domain";
    if (!MonoResolver::Instance().Init())
    {
        m_lastError = MonoResolver::Instance().LastError();
        return false;
    }
    // 管道命令始终在同一个 DLL 工作线程串行执行。这里附加一次并保持到
    // Shutdown，避免每个 Binding 重复 attach/detach 或在未附加状态调用 API。
    m_lastStage = "attaching the DLL worker thread to the Mono Root Domain";
    m_workerThread = MonoResolver::Instance().AttachThread();
    if (!m_workerThread)
    {
        m_lastError = "mono_thread_attach returned null for the DLL worker thread";
        MonoResolver::Instance().Shutdown();
        return false;
    }
    m_domain = MonoResolver::Instance().CurrentDomain();
    if (!m_domain)
    {
        m_lastError = "mono_domain_get returned null after thread attachment";
        Shutdown();
        return false;
    }

    m_initialized = true;
    m_lastStage = "enumerating loaded Mono assemblies";
    if (!RefreshAssemblies())
    {
        m_lastError = "failed to enumerate loaded Mono assemblies";
        Shutdown();
        return false;
    }
    m_lastStage = "Mono runtime initialization completed";
    return true;
}

void MonoRuntime::CollectAssembly(MonoAssembly* assembly, void* userData)
{
    // mono_assembly_foreach 要求 C 风格回调，因此只在回调中收集借用指针，
    // 不获取 Runtime 互斥锁，避免回调内部与外层锁形成反向锁序。
    auto* output = static_cast<std::vector<MonoAssemblyInfo>*>(userData);
    auto& resolver = MonoResolver::Instance();
    MonoRuntime::Instance().m_lastStage =
        "calling mono_assembly_get_image during assembly enumeration";
    MonoImage* image = resolver.AssemblyImage(assembly);
    MonoRuntime::Instance().m_lastStage =
        "calling mono_image_get_name during assembly enumeration";
    const char* name = resolver.ImageName(image);
    if (assembly && image && name && *name) output->push_back({assembly, image, name, 0});
}

bool MonoRuntime::RefreshAssemblies()
{
    if (!m_initialized) return false;
    // 先在锁外建立完整快照，成功后一次性交换。这样查询方永远不会看到
    // 枚举到一半的程序集列表。
    std::vector<MonoAssemblyInfo> fresh;
    m_lastStage = "calling mono_assembly_foreach";
    MonoResolver::Instance().EnumerateAssemblies(CollectAssembly, &fresh);
    m_lastStage = "publishing the Mono assembly snapshot";
    bool invalidated = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& previous : m_assemblies)
        {
            const auto found = std::find_if(fresh.begin(), fresh.end(),
                [&previous](const MonoAssemblyInfo& current)
                {
                    return current.assembly == previous.assembly &&
                        current.image == previous.image;
                });
            if (found == fresh.end()) { invalidated = true; break; }
        }
        if (invalidated) ++m_generation;
        const uint64_t generation = m_generation.load();
        for (auto& assembly : fresh) assembly.generation = generation;
        m_assemblies = std::move(fresh);
    }
    if (invalidated)
    {
        // 已卸载 Image 对应的 Method 地址不能继续作为 Hook 入口。这里只
        // 禁用并保留底层分配，最终 Shutdown 等待在途回调后统一释放。
        MonoHook::InvalidateMetadata();
        MonoScheduler::InvalidateMetadata();
    }
    return true;
}

std::vector<MonoAssemblyInfo> MonoRuntime::Assemblies() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_assemblies;
}

std::string MonoRuntime::NormalizeAssemblyName(std::string name)
{
    // Mono image 通常返回 Assembly-CSharp.dll。公开 API 同时接受带扩展名
    // 和不带扩展名的形式，并使用 ASCII 大小写无关比较。
    std::transform(
        name.begin(), name.end(), name.begin(),
        [](unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
    if (name.size() > 4 && name.substr(name.size() - 4) == ".dll") name.resize(name.size() - 4);
    return name;
}

bool MonoRuntime::FindAssembly(const std::string& name, MonoAssemblyInfo& result) const
{
    const std::string wanted = NormalizeAssemblyName(name);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_assemblies)
    {
        if (NormalizeAssemblyName(item.name) == wanted) { result = item; return true; }
    }
    return false;
}

bool MonoRuntime::FindAssemblyByImage(MonoImage* image, MonoAssemblyInfo& result) const
{
    if (!image) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_assemblies)
    {
        if (item.image == image)
        {
            result = item;
            return true;
        }
    }
    return false;
}

MonoClass* MonoRuntime::FindClass(const char* nameSpace, const char* name) const
{
    if (!nameSpace || !name) return nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& resolver = MonoResolver::Instance();
    for (const auto& assembly : m_assemblies)
    {
        MonoClass* klass = resolver.FindClass(assembly.image, nameSpace, name);
        if (klass) return klass;
    }
    return nullptr;
}

bool MonoRuntime::FindUnityObjects(
    MonoClass* klass, std::vector<uint32_t>& handles, std::string& error) const
{
    handles.clear();
    error.clear();
    if (!klass) { error = "class is null"; return false; }

    auto& resolver = MonoResolver::Instance();
    MonoClass* unityObject = FindClass("UnityEngine", "Object");
    if (!unityObject)
    {
        error = "UnityEngine.Object was not found in the loaded assemblies";
        return false;
    }

    bool derivesFromUnityObject = false;
    for (MonoClass* current = klass; current; current = resolver.ClassParent(current))
    {
        if (current == unityObject) { derivesFromUnityObject = true; break; }
    }
    if (!derivesFromUnityObject)
    {
        error = "class does not derive from UnityEngine.Object";
        return false;
    }

    MonoObject* typeObject = resolver.TypeObject(resolver.ClassType(klass));
    if (!typeObject) { error = "failed to create System.Type for class"; return false; }

    MonoMethod* findMethod = nullptr;
    bool usesSortMode = false;
    for (MonoMethod* method : resolver.EnumerateMethods(unityObject))
    {
        const char* name = resolver.MethodName(method);
        if (!name || strcmp(name, "FindObjectsOfType") != 0) continue;
        if ((resolver.MethodFlags(method) & 0x0010) == 0) continue;
        const auto parameterNames = resolver.MethodParameterTypes(method);
        if (parameterNames.size() == 1 && parameterNames[0] == "System.Type")
        {
            findMethod = method;
            break;
        }
        if (!findMethod && parameterNames.size() == 2 &&
            parameterNames[0] == "System.Type" && parameterNames[1] == "System.Boolean")
        {
            findMethod = method;
        }
    }
    // Unity 2023+ 逐步以 FindObjectsByType(Type, FindObjectsSortMode)
    // 取代旧入口。None 的枚举底层值为 0，与 Il2CppLua 的兼容策略一致。
    if (!findMethod)
    {
        for (MonoMethod* method : resolver.EnumerateMethods(unityObject))
        {
            const char* name = resolver.MethodName(method);
            if (!name || strcmp(name, "FindObjectsByType") != 0) continue;
            if ((resolver.MethodFlags(method) & 0x0010) == 0) continue;
            const auto parameterNames = resolver.MethodParameterTypes(method);
            if (parameterNames.size() == 2 && parameterNames[0] == "System.Type" &&
                parameterNames[1].find("FindObjectsSortMode") != std::string::npos)
            {
                findMethod = method;
                usesSortMode = true;
                break;
            }
        }
    }
    if (!findMethod)
    {
        error = "UnityEngine.Object.FindObjectsOfType/FindObjectsByType was not found";
        return false;
    }

    uint8_t includeInactive = 0;
    int32_t sortModeNone = 0;
    void* parameters[2] = {typeObject, usesSortMode
        ? static_cast<void*>(&sortModeNone) : static_cast<void*>(&includeInactive)};
    MonoObject* exception = nullptr;
    MonoObject* result = resolver.Invoke(
        findMethod, nullptr, parameters, &exception);
    if (exception)
    {
        error = resolver.ObjectString(exception);
        if (error.empty()) error = "FindObjectsOfType threw a managed exception";
        return false;
    }
    if (!result) return true;

    // 遍历期间创建每个元素的 Lua GC handle 可能触发分配，因此先用临时
    // handle 固定数组的托管引用；每次循环重新解析目标以适应移动式 GC。
    const uint32_t arrayHandle = resolver.CreateGCHandle(result);
    if (!arrayHandle) { error = "failed to retain FindObjectsOfType result array"; return false; }
    MonoArray* array = reinterpret_cast<MonoArray*>(resolver.GCHandleTarget(arrayHandle));
    const uintptr_t length = resolver.ArrayLength(array);
    handles.reserve(static_cast<size_t>(length));
    for (uintptr_t index = 0; index < length; ++index)
    {
        array = reinterpret_cast<MonoArray*>(resolver.GCHandleTarget(arrayHandle));
        MonoObject* object = resolver.ArrayElement(array, index);
        if (!object) continue;
        const uint32_t objectHandle = resolver.CreateGCHandle(object);
        if (!objectHandle)
        {
            for (uint32_t handle : handles) resolver.FreeGCHandle(handle);
            handles.clear();
            resolver.FreeGCHandle(arrayHandle);
            error = "failed to retain a Unity object result";
            return false;
        }
        handles.push_back(objectHandle);
    }
    resolver.FreeGCHandle(arrayHandle);
    return true;
}

std::string MonoRuntime::Status() const
{
    const auto& resolver = MonoResolver::Instance();
    std::ostringstream output;
    output << "Initialized: " << (m_initialized ? "true" : "false") << '\n';
    output << "Exports: " << resolver.ResolveStatus() << '\n';
    output << "Assemblies: " << Assemblies().size() << '\n';
    output << "Root domain: 0x" << std::uppercase << std::hex << reinterpret_cast<uintptr_t>(resolver.RootDomain()) << '\n';
    output << "Current domain: 0x" << std::uppercase << std::hex << reinterpret_cast<uintptr_t>(m_domain) << '\n';
    output << "Metadata generation: " << std::dec << Generation() << '\n';
    output << "Mono: 0x" << std::uppercase << std::hex << reinterpret_cast<uintptr_t>(resolver.Module());
    return output.str();
}

void MonoRuntime::Shutdown()
{
    // LuaEngine 必须先销毁 Instance userdata 并释放 GC handle，再清空 Resolver；
    // Hook 加入后仍保持 Hook→Lua→Resolver 的关闭顺序。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_assemblies.clear();
    m_initialized = false;
    MonoResolver::Instance().DetachThread(m_workerThread);
    m_workerThread = nullptr;
    m_domain = nullptr;
    ++m_generation;
    MonoResolver::Instance().Shutdown();
}
