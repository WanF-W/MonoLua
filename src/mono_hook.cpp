/**
 * ============================================================
 * mono_hook.cpp — Mono JIT 方法 Hook 管理实现
 * ============================================================
 * 每个方法分配携带 hookId 的小型 thunk，公共 MASM 入口保存 Windows x64
 * 易失参数寄存器后进入 C++ 分发。original() 通过线程局部旁路进入 MinHook
 * trampoline，避免全局停用 Hook 造成其他游戏线程漏过回调。
 * ============================================================
 */
#include "mono_hook.h"
#include "hook_stub.h"
#include "mono_resolver.h"
#include "mono_scheduler.h"
#include "lua_binding_internal.h"
#include "lua_engine.h"
#include "../minhook_src/MinHook.h"
#include <memory>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

struct NativeHookContext
{
    uint64_t rcx, rdx, r8, r9;
    uint64_t xmm0, xmm1, xmm2, xmm3;
    uint64_t stackArgs, hookId, resultInt, resultFloat;
    void* original;
    int32_t hasResult, reserved;
};
static_assert(sizeof(NativeHookContext) == 0x70, "NativeHookContext layout mismatch");

namespace
{
    constexpr uint32_t METHOD_STATIC = 0x0010;
    constexpr int TYPE_VOID = 0x01, TYPE_BOOLEAN = 0x02, TYPE_CHAR = 0x03;
    constexpr int TYPE_U8 = 0x0B, TYPE_R4 = 0x0C, TYPE_R8 = 0x0D, TYPE_STRING = 0x0E;
    constexpr int TYPE_BYREF = 0x10, TYPE_VALUETYPE = 0x11, TYPE_CLASS = 0x12;
    constexpr int TYPE_ARRAY = 0x14, TYPE_GENERICINST = 0x15, TYPE_I = 0x18, TYPE_U = 0x19;
    constexpr int TYPE_OBJECT = 0x1C, TYPE_SZARRAY = 0x1D;
    constexpr size_t THUNK_SIZE = 32;

    struct HookEntry
    {
        MonoMethod* method = nullptr;
        void* target = nullptr;
        void* thunk = nullptr;
        void* original = nullptr;
        uint32_t id = 0;
        int luaRef = LUA_REFNIL;
        bool enabled = false;
        bool internalTick = false;
        bool isStatic = false;
        std::vector<MonoType*> parameters;
        MonoType* returnType = nullptr;
    };

    struct OriginalState
    {
        HookEntry* entry = nullptr;
        uint32_t objectHandle = 0;
        std::vector<uint64_t> arguments;
        std::vector<uint32_t> argumentHandles;
        bool active = true;
    };

    std::mutex g_mutex;
    std::vector<std::unique_ptr<HookEntry>> g_entries;
    std::map<MonoMethod*, HookEntry*> g_methods;
    bool g_minHookReady = false;
    std::atomic<bool> g_shutdown{false};
    std::atomic<uint32_t> g_activeDispatches{0};
    HookEntry* g_tickEntry = nullptr;
    thread_local HookEntry* g_bypassEntry = nullptr;
    thread_local OriginalState* g_currentOriginal = nullptr;

    bool IsFloat(MonoType* type)
    {
        const int kind = MonoResolver::Instance().TypeKind(type);
        return kind == TYPE_R4 || kind == TYPE_R8;
    }

    bool IsReference(MonoType* type)
    {
        auto& resolver = MonoResolver::Instance();
        const int kind = resolver.TypeKind(type);
        return kind == TYPE_STRING || kind == TYPE_CLASS || kind == TYPE_ARRAY ||
            kind == TYPE_OBJECT || kind == TYPE_SZARRAY;
    }

    void ReleaseOriginalHandles(OriginalState& original)
    {
        auto& resolver = MonoResolver::Instance();
        for (uint32_t handle : original.argumentHandles)
            if (handle) resolver.FreeGCHandle(handle);
        original.argumentHandles.clear();
        if (original.objectHandle) resolver.FreeGCHandle(original.objectHandle);
        original.objectHandle = 0;
    }

    bool IsSupported(MonoType* type, bool allowVoid)
    {
        auto& resolver = MonoResolver::Instance();
        if (!type) return false;
        const int kind = resolver.TypeKind(type);
        if (allowVoid && kind == TYPE_VOID) return true;
        if (resolver.TypeIsByRef(type) || kind == TYPE_BYREF) return false;
        if (kind == TYPE_VALUETYPE)
        {
            MonoClass* klass = resolver.TypeClass(type);
            return klass && resolver.ClassIsEnum(klass);
        }
        if (kind == TYPE_GENERICINST)
        {
            // 泛型共享入口可能携带隐藏的 rgctx 参数，不能按普通 Windows
            // x64 参数槽解释。普通 runtime_invoke 不受此 Hook 限制。
            return false;
        }
        return (kind >= TYPE_BOOLEAN && kind <= TYPE_STRING) || kind == TYPE_CLASS ||
            kind == TYPE_ARRAY || kind == TYPE_I || kind == TYPE_U ||
            kind == TYPE_OBJECT || kind == TYPE_SZARRAY;
    }

    bool EnsureMinHook()
    {
        if (g_minHookReady) return true;
        const MH_STATUS status = MH_Initialize();
        g_minHookReady = status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
        return g_minHookReady;
    }

    void* AllocateThunk(uint32_t id)
    {
        auto* memory = static_cast<uint8_t*>(VirtualAlloc(
            nullptr, THUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!memory) return nullptr;
        memory[0] = 0xB8;
        memcpy(memory + 1, &id, sizeof(id));
        memory[5] = 0xFF; memory[6] = 0x25; memset(memory + 7, 0, 4);
        void* detour = GetHookDetourAddress();
        memcpy(memory + 11, &detour, sizeof(detour));
        FlushInstructionCache(GetCurrentProcess(), memory, THUNK_SIZE);
        return memory;
    }

    uint64_t ReadSlot(const NativeHookContext* context, int position, bool floating)
    {
        if (position < 4)
        {
            const uint64_t* registers = floating ? &context->xmm0 : &context->rcx;
            return registers[position];
        }
        return reinterpret_cast<const uint64_t*>(context->stackArgs)[position - 4];
    }

    struct ProtectedInvokeState { MonoMethod* method; MonoObject* object; };

    int ProtectedInvoke(lua_State* state)
    {
        auto* invoke = static_cast<ProtectedInvokeState*>(lua_touserdata(state, lua_upvalueindex(1)));
        return LuaBridge_InvokeMethod(state, invoke->method, invoke->object, 1);
    }

    int OriginalInvoke(lua_State* state)
    {
        OriginalState* call = g_currentOriginal;
        if (!call || !call->active || !call->entry)
            return luaL_error(state, "original() is only valid during its hook callback");
        if (lua_gettop(state) == 0)
        {
            for (size_t i = 0; i < call->entry->parameters.size(); ++i)
            {
                uint64_t raw = call->arguments[i];
                if (call->argumentHandles[i])
                    raw = reinterpret_cast<uint64_t>(
                        MonoResolver::Instance().GCHandleTarget(call->argumentHandles[i]));
                std::string error;
                if (!LuaBridge_PushRawValue(
                    state, call->entry->parameters[i], &raw, error))
                    return luaL_error(state, "%s", error.c_str());
            }
        }
        const int argumentCount = lua_gettop(state);
        MonoObject* object = call->objectHandle
            ? MonoResolver::Instance().GCHandleTarget(call->objectHandle) : nullptr;
        ProtectedInvokeState invoke{call->entry->method, object};
        lua_pushlightuserdata(state, &invoke);
        lua_pushcclosure(state, ProtectedInvoke, 1);
        lua_insert(state, 1);
        HookEntry* previousBypass = g_bypassEntry;
        g_bypassEntry = call->entry;
        const int status = lua_pcall(state, argumentCount, LUA_MULTRET, 0);
        g_bypassEntry = previousBypass;
        if (status != LUA_OK) return lua_error(state);
        return lua_gettop(state);
    }

    bool SetResult(lua_State* state, int index, MonoType* type, NativeHookContext* context)
    {
        auto& resolver = MonoResolver::Instance();
        int kind = resolver.TypeKind(type);
        if (kind == TYPE_VALUETYPE) kind = resolver.TypeKind(resolver.EnumBaseType(resolver.TypeClass(type)));
        if (kind == TYPE_VOID) return true;
        if (kind == TYPE_R4) { float value = static_cast<float>(lua_tonumber(state, index)); memcpy(&context->resultFloat, &value, 4); return true; }
        if (kind == TYPE_R8) { double value = lua_tonumber(state, index); memcpy(&context->resultFloat, &value, 8); return true; }
        if (kind == TYPE_BOOLEAN) { context->resultInt = lua_toboolean(state, index) != 0; return true; }
        if ((kind >= TYPE_CHAR && kind <= TYPE_U8) || kind == TYPE_I || kind == TYPE_U)
        { context->resultInt = static_cast<uint64_t>(lua_tointeger(state, index)); return true; }
        if (kind == TYPE_STRING)
        {
            context->resultInt = lua_isnil(state, index) ? 0 : reinterpret_cast<uint64_t>(
                resolver.NewString(resolver.CurrentDomain(), lua_tostring(state, index)));
            return true;
        }
        if (lua_isnil(state, index)) { context->resultInt = 0; return true; }
        if (!luaL_testudata(state, index, LuaBridgeMT::INSTANCE)) return false;
        context->resultInt = reinterpret_cast<uint64_t>(LuaBridge_GetInstanceObject(state, index));
        return true;
    }

    void Dispatch(NativeHookContext* context)
    {
        HookEntry* entry = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (context->hookId < g_entries.size()) entry = g_entries[context->hookId].get();
        }
        if (!entry || !entry->enabled || g_shutdown.load() || g_bypassEntry == entry)
        {
            context->original = entry ? entry->original : nullptr;
            return;
        }
        if (entry->internalTick)
        {
            MonoScheduler::OnTick();
            if (entry->luaRef == LUA_REFNIL) { context->original = entry->original; return; }
        }

        auto& engine = LuaEngine::Instance();
        std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
        lua_State* state = engine.GetState();
        if (!state) { context->original = entry->original; return; }
        const int base = lua_gettop(state);
        OriginalState original;
        original.entry = entry;
        int position = 0;
        if (!entry->isStatic)
        {
            MonoObject* object = reinterpret_cast<MonoObject*>(
                ReadSlot(context, position++, false));
            original.objectHandle = MonoResolver::Instance().CreateGCHandle(object, true);
            if (!original.objectHandle)
            {
                lua_settop(state, base);
                context->original = entry->original;
                return;
            }
        }
        original.argumentHandles.resize(entry->parameters.size(), 0);
        for (size_t index = 0; index < entry->parameters.size(); ++index)
        {
            MonoType* type = entry->parameters[index];
            const uint64_t raw = ReadSlot(context, position++, IsFloat(type));
            original.arguments.push_back(raw);
            if (raw && IsReference(type))
            {
                original.argumentHandles[index] = MonoResolver::Instance().CreateGCHandle(
                    reinterpret_cast<MonoObject*>(raw), true);
                if (!original.argumentHandles[index])
                {
                    ReleaseOriginalHandles(original);
                    lua_settop(state, base);
                    context->original = entry->original;
                    return;
                }
            }
        }

        lua_rawgeti(state, LUA_REGISTRYINDEX, entry->luaRef);
        if (entry->isStatic) LuaBridge_PushClass(state, MonoResolver::Instance().MethodClass(entry->method));
        else LuaBridge_PushInstance(state,
            MonoResolver::Instance().GCHandleTarget(original.objectHandle));
        lua_pushcfunction(state, OriginalInvoke);
        for (size_t i = 0; i < entry->parameters.size(); ++i)
        {
            uint64_t raw = original.arguments[i];
            if (original.argumentHandles[i])
                raw = reinterpret_cast<uint64_t>(MonoResolver::Instance().GCHandleTarget(
                    original.argumentHandles[i]));
            std::string error;
            if (!LuaBridge_PushRawValue(state, entry->parameters[i], &raw, error))
            {
                ReleaseOriginalHandles(original);
                lua_settop(state, base);
                context->original = entry->original;
                return;
            }
        }
        OriginalState* previousOriginal = g_currentOriginal;
        g_currentOriginal = &original;
        const int callbackStatus = lua_pcall(
            state, 2 + static_cast<int>(entry->parameters.size()), LUA_MULTRET, 0);
        g_currentOriginal = previousOriginal;
        if (callbackStatus != LUA_OK)
        {
            ReleaseOriginalHandles(original);
            lua_settop(state, base);
            context->original = entry->original;
            return;
        }
        original.active = false;
        ReleaseOriginalHandles(original);
        const int returnKind = MonoResolver::Instance().TypeKind(entry->returnType);
        if (returnKind != TYPE_VOID && lua_gettop(state) == base)
        { context->original = entry->original; return; }
        if (returnKind != TYPE_VOID && !SetResult(state, base + 1, entry->returnType, context))
        { lua_settop(state, base); context->original = entry->original; return; }
        lua_settop(state, base); context->original = nullptr;
    }

    void* FindOriginal(uint64_t hookId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return hookId < g_entries.size() && g_entries[hookId]
            ? g_entries[hookId]->original : nullptr;
    }
}

extern "C" void HookDispatch(NativeHookContext* context)
{
    ++g_activeDispatches;
    __try { Dispatch(context); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_currentOriginal = nullptr;
        g_bypassEntry = nullptr;
        if (context) context->original = FindOriginal(context->hookId);
    }
    --g_activeDispatches;
}

bool MonoHook::HookMethod(lua_State* state, MonoMethod* method, int callbackIndex, std::string& error)
{
    error.clear();
    if (!method || lua_type(state, callbackIndex) != LUA_TFUNCTION) { error = "hook requires a function"; return false; }
    auto& resolver = MonoResolver::Instance();
    MonoClass* declaringClass = resolver.MethodClass(method);
    if (!declaringClass || resolver.ClassIsValueType(declaringClass))
    {
        error = "hooking methods declared by value types is not supported";
        return false;
    }
    // 老版本 Mono 没有 mono_method_is_generic/inflated。MethodIsGeneric()
    // 在缺失这些可选导出时保守返回 false；不能因此让所有普通方法 Hook
    // 都不可用。真正无法安全识别的泛型方法仍由签名检查拒绝。
    if (resolver.MethodIsGeneric(method))
    {
        error = "hooking generic or inflated methods is not supported";
        return false;
    }
    const auto parameters = resolver.MethodParameters(method);
    for (MonoType* type : parameters) if (!IsSupported(type, false))
    { error = "method contains an unsupported struct or ref/out parameter"; return false; }
    MonoType* returnType = resolver.MethodReturnType(method);
    if (!IsSupported(returnType, true)) { error = "method has an unsupported struct return type"; return false; }
    void* target = resolver.CompileMethod(method);
    if (!target || !EnsureMinHook()) { error = "failed to prepare native hook"; return false; }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto existing = g_methods.find(method);
    if (existing != g_methods.end())
    {
        HookEntry* entry = existing->second;
        if (entry->luaRef != LUA_REFNIL) { error = "method is already hooked"; return false; }
        entry->parameters = parameters;
        entry->returnType = returnType;
        entry->isStatic = (resolver.MethodFlags(method) & METHOD_STATIC) != 0;
        lua_pushvalue(state, callbackIndex);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        const MH_STATUS status = MH_EnableHook(entry->target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
            error = "failed to re-enable method hook";
            return false;
        }
        entry->luaRef = reference;
        entry->enabled = true; return true;
    }
    // 泛型共享代码可能让两个 MonoMethod 编译到同一原生入口。一个入口
    // 只能安装一个 thunk；不同 Method 的签名/声明类无法可靠区分，因此
    // 明确拒绝，避免 MinHook 返回含糊错误或把回调绑定到错误的方法。
    for (const auto& item : g_entries)
    {
        if (item && item->target == target)
        {
            error = "method shares its native address with another registered Mono method";
            return false;
        }
    }
    auto entry = std::make_unique<HookEntry>();
    entry->method = method; entry->target = target; entry->id = static_cast<uint32_t>(g_entries.size());
    entry->parameters = parameters; entry->returnType = returnType;
    entry->isStatic = (resolver.MethodFlags(method) & METHOD_STATIC) != 0;
    lua_pushvalue(state, callbackIndex); entry->luaRef = luaL_ref(state, LUA_REGISTRYINDEX);
    entry->thunk = AllocateThunk(entry->id);
    const bool hasThunk = entry->thunk != nullptr;
    const MH_STATUS createStatus = hasThunk
        ? MH_CreateHook(target, entry->thunk, &entry->original) : MH_ERROR_MEMORY_ALLOC;
    const MH_STATUS enableStatus = createStatus == MH_OK ? MH_EnableHook(target) : MH_UNKNOWN;
    if (!hasThunk || createStatus != MH_OK || enableStatus != MH_OK)
    {
        if (createStatus == MH_OK) MH_RemoveHook(target);
        if (entry->thunk) VirtualFree(entry->thunk, 0, MEM_RELEASE);
        luaL_unref(state, LUA_REGISTRYINDEX, entry->luaRef);
        error = "failed to install native method hook"; return false;
    }
    entry->enabled = true; g_methods[method] = entry.get(); g_entries.push_back(std::move(entry));
    return true;
}

bool MonoHook::UnhookMethod(MonoMethod* method)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_methods.find(method);
    if (found == g_methods.end() || found->second->luaRef == LUA_REFNIL) return false;
    HookEntry* entry = found->second;
    lua_State* state = LuaEngine::Instance().GetState();
    if (state) luaL_unref(state, LUA_REGISTRYINDEX, entry->luaRef);
    entry->luaRef = LUA_REFNIL;
    if (!entry->internalTick) { MH_DisableHook(entry->target); entry->enabled = false; }
    return true;
}

bool MonoHook::IsHooked(MonoMethod* method)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_methods.find(method);
    return found != g_methods.end() && found->second->enabled && found->second->luaRef != LUA_REFNIL;
}

bool MonoHook::InstallTick(MonoMethod* method, std::string& error)
{
    error.clear();
    if (!method) { error = "tick method is null"; return false; }
    auto& resolver = MonoResolver::Instance();
    void* target = resolver.CompileMethod(method);
    if (!target || !EnsureMinHook()) { error = "failed to prepare tick hook"; return false; }
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_tickEntry && g_tickEntry->target != target)
    {
        g_tickEntry->internalTick = false;
        if (g_tickEntry->luaRef == LUA_REFNIL)
        { MH_DisableHook(g_tickEntry->target); g_tickEntry->enabled = false; }
        g_tickEntry = nullptr;
    }
    for (auto& item : g_entries)
    {
        if (!item || item->target != target) continue;
        item->internalTick = true;
        g_methods[method] = item.get();
        const MH_STATUS status = MH_EnableHook(target);
        if (status != MH_OK && status != MH_ERROR_ENABLED) { error = "failed to enable tick hook"; return false; }
        item->enabled = true; g_tickEntry = item.get(); return true;
    }
    auto entry = std::make_unique<HookEntry>();
    entry->method = method; entry->target = target; entry->id = static_cast<uint32_t>(g_entries.size());
    entry->internalTick = true; entry->thunk = AllocateThunk(entry->id);
    const bool hasThunk = entry->thunk != nullptr;
    const MH_STATUS createStatus = hasThunk
        ? MH_CreateHook(target, entry->thunk, &entry->original) : MH_ERROR_MEMORY_ALLOC;
    const MH_STATUS enableStatus = createStatus == MH_OK ? MH_EnableHook(target) : MH_UNKNOWN;
    if (!hasThunk || createStatus != MH_OK || enableStatus != MH_OK)
    {
        if (createStatus == MH_OK) MH_RemoveHook(target);
        if (entry->thunk) VirtualFree(entry->thunk, 0, MEM_RELEASE);
        error = "failed to install tick hook"; return false;
    }
    entry->enabled = true; g_tickEntry = entry.get(); g_methods[method] = entry.get(); g_entries.push_back(std::move(entry));
    return true;
}

void MonoHook::UnhookAll()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    lua_State* state = LuaEngine::Instance().GetState();
    for (auto& entry : g_entries)
    {
        if (!entry || entry->luaRef == LUA_REFNIL) continue;
        if (state) luaL_unref(state, LUA_REGISTRYINDEX, entry->luaRef);
        entry->luaRef = LUA_REFNIL;
        if (!entry->internalTick) { MH_DisableHook(entry->target); entry->enabled = false; }
    }
}

void MonoHook::InvalidateMetadata()
{
    auto& engine = LuaEngine::Instance();
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    std::lock_guard<std::mutex> lock(g_mutex);
    lua_State* state = engine.GetState();
    for (auto& entry : g_entries)
    {
        if (!entry) continue;
        if (entry->enabled) MH_DisableHook(entry->target);
        if (state && entry->luaRef != LUA_REFNIL)
            luaL_unref(state, LUA_REGISTRYINDEX, entry->luaRef);
        entry->luaRef = LUA_REFNIL;
        entry->enabled = false;
        entry->internalTick = false;
    }
    g_methods.clear();
    g_tickEntry = nullptr;
}

void MonoHook::Shutdown()
{
    g_shutdown.store(true);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry : g_entries)
            if (entry && entry->enabled) MH_DisableHook(entry->target);
    }

    // Disable 只阻止新的入口，已经进入 detour 的线程仍可能持有 HookEntry
    // 和 Lua 引用。必须等这些回调离开后才能释放 thunk 或关闭 Lua VM。
    while (g_activeDispatches.load() != 0) Sleep(1);

    auto& engine = LuaEngine::Instance();
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    std::lock_guard<std::mutex> lock(g_mutex);
    lua_State* state = engine.GetState();
    for (auto& entry : g_entries)
    {
        if (!entry) continue;
        MH_RemoveHook(entry->target);
        if (state && entry->luaRef != LUA_REFNIL)
            luaL_unref(state, LUA_REGISTRYINDEX, entry->luaRef);
        if (entry->thunk) VirtualFree(entry->thunk, 0, MEM_RELEASE);
    }
    g_entries.clear();
    g_methods.clear();
    g_tickEntry = nullptr;
    if (g_minHookReady) MH_Uninitialize();
    g_minHookReady = false;
}
