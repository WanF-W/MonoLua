/**
 * mono_runtime.cpp — Mono 高层运行时实现
 * 负责把回调式 Mono Assembly 枚举转换为稳定的 C++ 快照，并向 Binding
 * 提供大小写无关、可省略 .dll 的查找语义。快照变化同步 metadata
 * generation；对象创建和静态字段通过调用线程当前 Domain 完成。
 */
#include "mono_runtime.h"
#include "mono_resolver.h"
#include "mono_handle.h"
#include "mono_metadata.h"

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
    return current ? current : m_workerDomain.load();
}

bool MonoRuntime::Init()
{
    if (m_initialized.load()) return true;
    m_lastError.clear();
    if (!MonoResolver::Instance().Init())
    {
        m_lastError = MonoResolver::Instance().LastError();
        return false;
    }
    // 初始化阶段临时附加；开始等待管道前脱离，命令执行期间再按作用域附加。
    m_workerThread = MonoResolver::Instance().AttachThread();
    if (!m_workerThread)
    {
        m_lastError = "mono_thread_attach returned null for the DLL worker thread";
        MonoResolver::Instance().Shutdown();
        return false;
    }
    m_workerDomain.store(MonoResolver::Instance().CurrentDomain());
    if (!m_workerDomain.load())
    {
        m_lastError = "mono_domain_get returned null after thread attachment";
        Shutdown();
        return false;
    }

    m_initialized.store(true);
    if (!RefreshAssemblies())
    {
        m_lastError = "failed to enumerate loaded Mono assemblies";
        Shutdown();
        return false;
    }
    return true;
}

void MonoRuntime::CollectAssembly(MonoAssembly* assembly, void* userData)
{
    // mono_assembly_foreach 要求 C 风格回调，因此只在回调中收集借用指针，
    // 不获取 Runtime 互斥锁，避免回调内部与外层锁形成反向锁序。
    auto* output = static_cast<std::vector<MonoAssemblyInfo>*>(userData);
    auto& resolver = MonoResolver::Instance();
    MonoImage* image = resolver.AssemblyImage(assembly);
    const char* name = resolver.ImageName(image);
    if (assembly && image && name && *name) output->push_back({assembly, image, name, 0});
}

bool MonoRuntime::RefreshAssemblies()
{
    if (!m_initialized.load()) return false;
    // 先在锁外建立完整快照，成功后一次性交换。这样查询方永远不会看到
    // 枚举到一半的程序集列表。
    std::vector<MonoAssemblyInfo> fresh;
    MonoResolver::Instance().EnumerateAssemblies(CollectAssembly, &fresh);
    bool invalidated = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Thread-local domains may differ without any metadata reload.
        // Only a removed assembly invalidates the published snapshot.
        for (const auto& previous : m_assemblies)
        {
            const auto found =
                std::find_if(fresh.begin(), fresh.end(), [&previous](const MonoAssemblyInfo& current) {
                    return current.assembly == previous.assembly && current.image == previous.image;
                });
            if (found == fresh.end())
            {
                invalidated = true;
                break;
            }
        }
        if (invalidated)
        {
            ++m_generation;
            m_metadataChanged = true;
        }
        const uint64_t generation = m_generation.load();
        for (auto& assembly : fresh)
            assembly.generation = generation;
        m_assemblies = std::move(fresh);
    }
    return true;
}

bool MonoRuntime::TakeMetadataChanged()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool changed = m_metadataChanged;
    m_metadataChanged = false;
    return changed;
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
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (name.size() > 4 && name.substr(name.size() - 4) == ".dll") name.resize(name.size() - 4);
    return name;
}

bool MonoRuntime::FindAssembly(const std::string& name, MonoAssemblyInfo& result) const
{
    const std::string wanted = NormalizeAssemblyName(name);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_assemblies)
    {
        if (NormalizeAssemblyName(item.name) == wanted)
        {
            result = item;
            return true;
        }
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

bool MonoRuntime::InspectMethodGenerics(MonoMethod* method, bool& isGeneric) const
{
    auto& resolver = MonoResolver::Instance();
    isGeneric = resolver.TypeKind(resolver.ClassType(resolver.MethodClass(method))) ==
                mono_metadata::TYPE_GENERICINST;
    if (isGeneric) return true;
    if (resolver.CanInspectGenericMethods())
    {
        isGeneric = resolver.MethodIsGeneric(method);
        return true;
    }
    // Unity Mono versions do not consistently export the native generic predicates.
    // MethodInfo.IsGenericMethod includes both definitions and constructed methods.
    MonoObject* reflection = resolver.MethodObject(Domain(), method);
    if (!reflection) return false;
    mono::ScopedGCHandle root(resolver, resolver.CreateGCHandle(reflection, true));
    if (!root.value) return false;
    for (MonoClass* klass = resolver.ObjectClass(root.Target()); klass; klass = resolver.ClassParent(klass))
        for (MonoMethod* getter : resolver.EnumerateMethods(klass))
        {
            const char* name = resolver.MethodName(getter);
            if (!name || strcmp(name, "get_IsGenericMethod") != 0 ||
                !resolver.MethodParameters(getter).empty())
                continue;
            MonoObject* exception = nullptr;
            MonoObject* result = resolver.Invoke(getter, root.Target(), nullptr, &exception);
            if (exception || !result) return false;
            const auto* value = static_cast<const uint8_t*>(resolver.Unbox(result));
            if (!value) return false;
            isGeneric = *value != 0;
            return true;
        }
    return false;
}

bool MonoRuntime::InspectOpenMethod(MonoMethod* method, bool& containsGenericParameters) const
{
    const uint64_t generation = Generation();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_openMethodGeneration != generation)
        {
            m_openMethodCache.clear();
            m_openMethodGeneration = generation;
        }
        const auto cached = m_openMethodCache.find(method);
        if (cached != m_openMethodCache.end())
        {
            containsGenericParameters = cached->second;
            return true;
        }
    }
    // MethodBase.ContainsGenericParameters also covers an open declaring type.
    // IsGenericMethod alone would incorrectly reject constructed generic methods.
    auto& resolver = MonoResolver::Instance();
    MonoObject* reflection = resolver.MethodObject(Domain(), method);
    if (!reflection) return false;
    mono::ScopedGCHandle root(resolver, resolver.CreateGCHandle(reflection, true));
    if (!root.value) return false;
    for (MonoClass* klass = resolver.ObjectClass(root.Target()); klass; klass = resolver.ClassParent(klass))
        for (MonoMethod* getter : resolver.EnumerateMethods(klass))
        {
            const char* name = resolver.MethodName(getter);
            if (!name || strcmp(name, "get_ContainsGenericParameters") != 0 ||
                !resolver.MethodParameters(getter).empty()) continue;
            MonoObject* exception = nullptr;
            MonoObject* result = resolver.Invoke(getter, root.Target(), nullptr, &exception);
            if (exception || !result) return false;
            mono::ScopedGCHandle resultRoot(resolver, resolver.CreateGCHandle(result, true));
            if (!resultRoot.value) return false;
            const auto* value = static_cast<const uint8_t*>(resolver.Unbox(resultRoot.Target()));
            if (!value) return false;
            containsGenericParameters = *value != 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (Generation() != generation) return false;
                m_openMethodCache[method] = containsGenericParameters;
            }
            return true;
        }
    return false;
}

std::string MonoRuntime::Status() const
{
    const auto& resolver = MonoResolver::Instance();
    const auto assemblies = Assemblies();
    size_t imageCount = 0;
    for (const auto& assembly : assemblies)
        if (assembly.image) ++imageCount;
    std::ostringstream output;
    output << "Initialized: " << (m_initialized.load() ? "true" : "false") << '\n';
    output << "Exports: " << resolver.ResolveStatus() << '\n';
    output << "Assemblies: " << assemblies.size() << '\n';
    output << "Images: " << imageCount << '\n';
    output << "Root domain: 0x" << std::uppercase << std::hex
           << reinterpret_cast<uintptr_t>(resolver.RootDomain()) << '\n';
    output << "Worker domain: 0x" << std::uppercase << std::hex
           << reinterpret_cast<uintptr_t>(m_workerDomain.load()) << '\n';
    output << "Metadata generation: " << std::dec << Generation() << '\n';
    output << "Mono: 0x" << std::uppercase << std::hex << reinterpret_cast<uintptr_t>(resolver.Module());
    return output.str();
}

void MonoRuntime::Shutdown()
{
    // LuaEngine 必须先销毁 Instance userdata 并释放 GC handle，再清空 Resolver；
    // 正常关闭顺序为 Hook→Scheduler→Lua→Runtime→Resolver。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_assemblies.clear();
    m_initialized.store(false);
    m_metadataChanged = false;
    DetachInitializationThread();
    m_workerDomain.store(nullptr);
    ++m_generation;
    MonoResolver::Instance().Shutdown();
}

void MonoRuntime::DetachInitializationThread()
{
    if (m_workerThread) MonoResolver::Instance().DetachThread(m_workerThread);
    m_workerThread = nullptr;
}
