// Mono JIT Hook：thunk 标识方法，共享汇编保存参数，NativeInvoke 使用独立栈。
// original() 经 runtime_invoke 和线程局部旁路；已尝试原调用的回调不自动重试。
// Lua 锁在 Hook 表锁之前。原生回退在途期间同样保留 trampoline。
#include "mono_hook.h"
#include "hook_stub.h"
#include "mono_metadata.h"
#include "mono_resolver.h"
#include "mono_runtime.h"
#include "lua_binding_internal.h"
#include "lua_engine.h"
#include "lua_value_internal.h"
#include "mono_handle.h"
#include "../minhook_src/MinHook.h"
#include <memory>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

extern "C"
{
    volatile LONGLONG g_hookStubUsers = 0;
}

struct NativeHookContext
{
    uint64_t rcx, rdx, r8, r9;
    uint64_t xmm0, xmm1, xmm2, xmm3;
    uint64_t stackArgs, hookId, resultInt, resultFloat;
    void* original;
    uint64_t stackCount;
};
static_assert(sizeof(NativeHookContext) == 0x70, "NativeHookContext layout mismatch");
static_assert(offsetof(NativeHookContext, stackArgs) == 0x40);
static_assert(offsetof(NativeHookContext, resultInt) == 0x50);
static_assert(offsetof(NativeHookContext, original) == 0x60);
static_assert(offsetof(NativeHookContext, stackCount) == 0x68);

namespace
{
    using namespace mono_metadata;
    constexpr size_t THUNK_SIZE = 19;

    struct HookEntry
    {
        MonoMethod* method = nullptr;
        void* target = nullptr;
        void* thunk = nullptr;
        void* original = nullptr;
        uint32_t id = 0;
        int luaRef = LUA_REFNIL;
        bool enabled = false;
        void (*tickCallback)() = nullptr;
        bool isStatic = false;
        std::vector<MonoType*> parameters;
        MonoType* returnType = nullptr;
    };

    struct OriginalState
    {
        HookEntry* entry = nullptr;
        MonoMethod* method = nullptr;
        NativeHookContext* context = nullptr;
        uint64_t token = 0;
        bool invoked = false;
        uint32_t resultHandle = 0;
        std::vector<MonoType*> parameters;
        uint32_t objectHandle = 0;
        std::vector<uint64_t> arguments;
        std::vector<uint32_t> argumentHandles;
        bool active = true;
        OriginalState* previous = nullptr;
    };

    std::mutex g_mutex;
    std::vector<std::unique_ptr<HookEntry>> g_entries;
    std::map<MonoMethod*, HookEntry*> g_methods;
    bool g_minHookReady = false;
    std::atomic<bool> g_shutdown{false};
    std::vector<int> g_deferredLuaRefs;
    bool g_metadataCleanupPending = false;
    HookEntry* g_tickEntry = nullptr;
    thread_local HookEntry* g_bypassEntry = nullptr;
    thread_local OriginalState* g_currentOriginal = nullptr;
    uint64_t g_nextOriginalToken = 0; // protected by the Lua mutex

    bool HookStubsActive()
    {
        return InterlockedCompareExchange64(const_cast<LONGLONG*>(&g_hookStubUsers), 0, 0) != 0;
    }

    void WaitForHookStubs()
    {
        while (HookStubsActive())
            Sleep(1);
    }

    void QueueLuaRefLocked(int reference)
    {
        if (reference != LUA_REFNIL) g_deferredLuaRefs.push_back(reference);
    }

    bool IsFloat(MonoType* type)
    {
        const int kind = MonoResolver::Instance().TypeKind(type);
        return kind == TYPE_R4 || kind == TYPE_R8;
    }

    bool IsReference(MonoType* type)
    {
        auto& resolver = MonoResolver::Instance();
        return mono_value::IsReferenceType(resolver, type, resolver.TypeKind(type));
    }

    void ReleaseOriginalHandles(OriginalState& original)
    {
        auto& resolver = MonoResolver::Instance();
        for (uint32_t handle : original.argumentHandles)
            if (handle) resolver.FreeGCHandle(handle);
        original.argumentHandles.clear();
        if (original.objectHandle) resolver.FreeGCHandle(original.objectHandle);
        original.objectHandle = 0;
        resolver.FreeGCHandle(original.resultHandle);
        original.resultHandle = 0;
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
        return (kind >= TYPE_BOOLEAN && kind <= TYPE_STRING) || kind == TYPE_CLASS || kind == TYPE_ARRAY ||
               kind == TYPE_I || kind == TYPE_U || kind == TYPE_OBJECT || kind == TYPE_SZARRAY;
    }

    bool PrepareHook(MonoMethod* method, HookEntry& entry, std::string& error)
    {
        auto& resolver = MonoResolver::Instance();
        MonoClass* klass = resolver.MethodClass(method);
        if (!klass || resolver.ClassIsValueType(klass))
        {
            error = "hooking methods declared by value types is not supported";
            return false;
        }
        bool isGeneric = false;
        if (!MonoRuntime::Instance().InspectMethodGenerics(method, isGeneric))
        {
            error = "failed to inspect this method's generic calling convention";
            return false;
        }
        if (isGeneric)
        {
            error = "hooking generic or inflated methods is not supported";
            return false;
        }
        entry.method = method;
        entry.parameters = resolver.MethodParameters(method);
        entry.returnType = resolver.MethodReturnType(method);
        entry.isStatic = (resolver.MethodFlags(method) & METHOD_ATTRIBUTE_STATIC) != 0;
        if (entry.parameters.size() > 64)
        {
            error = "hook supports at most 64 parameters";
            return false;
        }
        for (MonoType* type : entry.parameters)
            if (!IsSupported(type, false))
            {
                error = "method contains an unsupported parameter type";
                return false;
            }
        if (!IsSupported(entry.returnType, true))
        {
            error = "method has an unsupported return type";
            return false;
        }
        entry.target = resolver.CompileMethod(method);
        if (!entry.target)
        {
            error = "failed to prepare native hook";
            return false;
        }
        return true;
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
        auto* memory = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, THUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!memory) return nullptr;
        memory[0] = 0xB8;
        memcpy(memory + 1, &id, sizeof(id));
        memory[5] = 0xFF;
        memory[6] = 0x25;
        memset(memory + 7, 0, 4);
        void* detour = GetHookDetourAddress();
        memcpy(memory + 11, &detour, sizeof(detour));
        FlushInstructionCache(GetCurrentProcess(), memory, THUNK_SIZE);
        return memory;
    }

    bool EnableNativeHook(HookEntry& entry, std::string& error)
    {
        entry.thunk = AllocateThunk(entry.id);
        if (!entry.thunk)
        {
            error = "failed to allocate native thunk";
            return false;
        }
        const MH_STATUS created = MH_CreateHook(entry.target, entry.thunk, &entry.original);
        const MH_STATUS enabled = created == MH_OK ? MH_EnableHook(entry.target) : MH_UNKNOWN;
        if (created != MH_OK || enabled != MH_OK)
        {
            if (created == MH_OK) MH_RemoveHook(entry.target);
            VirtualFree(entry.thunk, 0, MEM_RELEASE);
            entry.thunk = nullptr;
            error = "failed to install native hook";
            return false;
        }
        entry.enabled = true;
        return true;
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

    struct ProtectedInvokeState
    {
        MonoMethod* method;
        MonoObject* object;
        LuaMethodInvocation invocation;
    };

    int ProtectedInvoke(lua_State* state)
    {
        auto* invoke = static_cast<ProtectedInvokeState*>(lua_touserdata(state, lua_upvalueindex(1)));
        return LuaBridge_InvokeMethod(state, invoke->method, invoke->object, 1, &invoke->invocation);
    }

    bool SetResult(lua_State* state, int index, MonoType* type, NativeHookContext* context);

    int OriginalInvoke(lua_State* state)
    {
        OriginalState* call = g_currentOriginal;
        const uint64_t token = static_cast<uint64_t>(lua_tointeger(state, lua_upvalueindex(1)));
        while (call && call->token != token) call = call->previous;
        if (!call || !call->active || !call->entry || !call->method)
            return LuaEngine::RaiseBridgeError(state, "original() is only valid during its hook callback");
        const int argumentCount = lua_gettop(state);
        MonoObject* object =
            call->objectHandle ? MonoResolver::Instance().GCHandleTarget(call->objectHandle) : nullptr;
        ProtectedInvokeState invoke{call->method, object, {}};
        if (argumentCount == 0)
        {
            invoke.invocation.arguments = &call->arguments;
            invoke.invocation.handles = &call->argumentHandles;
        }
        invoke.invocation.entered = &call->invoked;
        invoke.invocation.resultHandle = &call->resultHandle;
        invoke.invocation.resultInt = &call->context->resultInt;
        invoke.invocation.resultFloat = &call->context->resultFloat;
        invoke.invocation.context = call->entry;
        invoke.invocation.beforeInvoke = [](void* entry) { g_bypassEntry = static_cast<HookEntry*>(entry); };
        lua_pushlightuserdata(state, &invoke);
        lua_pushcclosure(state, ProtectedInvoke, 1);
        lua_insert(state, 1);
        HookEntry* previousBypass = g_bypassEntry;
        const int status = lua_pcall(state, argumentCount, LUA_MULTRET, 0);
        g_bypassEntry = previousBypass;
        if (status != LUA_OK) return lua_error(state);
        // InvokeMethod retained both the raw completed result and its GC root.
        return lua_gettop(state);
    }

    bool SetResult(lua_State* state, int index, MonoType* type, NativeHookContext* context)
    {
        auto& resolver = MonoResolver::Instance();
        const int kind = mono_value::StorageTypeKind(resolver, type);
        if (kind == TYPE_VOID) return true;
        if ((kind >= TYPE_BOOLEAN && kind <= TYPE_R8) || kind == TYPE_I || kind == TYPE_U)
        {
            uint64_t value = 0;
            std::string error;
            if (!LuaBridge_ReadScalarValue(state, index, kind, &value, error)) return false;
            if (kind == TYPE_R4 || kind == TYPE_R8)
                context->resultFloat = value;
            else
                context->resultInt = value;
            return true;
        }
        if (kind == TYPE_STRING)
        {
            if (lua_isnil(state, index))
            {
                context->resultInt = 0;
                return true;
            }
            if (lua_type(state, index) != LUA_TSTRING) return false;
            const char* value = lua_tostring(state, index);
            MonoString* string =
                value ? resolver.NewString(resolver.CurrentDomain(), value, lua_rawlen(state, index))
                      : nullptr;
            if (!string) return false;
            context->resultInt = reinterpret_cast<uint64_t>(string);
            return true;
        }
        if (lua_isnil(state, index))
        {
            context->resultInt = 0;
            return true;
        }
        auto* userdata = static_cast<LuaInstanceUD*>(luaL_testudata(state, index, LuaBridgeMT::INSTANCE));
        if (!userdata || userdata->generation != MonoRuntime::Instance().Generation()) return false;
        MonoObject* object = resolver.GCHandleTarget(userdata->gcHandle);
        if (!object) return false;
        MonoClass* expected = resolver.TypeClass(type);
        if (expected && !resolver.ObjectIsInstanceOf(object, expected)) return false;
        context->resultInt = reinterpret_cast<uint64_t>(object);
        return true;
    }

    struct HookSnapshot
    {
        HookEntry* entry = nullptr;
        MonoMethod* method = nullptr;
        void* original = nullptr;
        int luaRef = LUA_REFNIL;
        void (*tickCallback)() = nullptr;
        bool isStatic = false;
        std::vector<MonoType*> parameters;
        MonoType* returnType = nullptr;
    };

    void ReportHookError(lua_State* state, const char* prefix)
    {
        std::string message = prefix ? prefix : "[hook] callback failed";
        if (state)
        {
            const std::string error = LuaEngine::ErrorText(state, -1);
            if (!error.empty())
            {
                message += ": ";
                message += error;
            }
        }
        message += '\n';
        LuaEngine::Instance().EmitOutput(message.c_str());
    }

    struct DispatchCleanup
    {
        OriginalState* state;

        ~DispatchCleanup()
        {
            if (!LuaEngine::Instance().IsFaulted()) ReleaseOriginalHandles(*state);
        }
    };

    struct ActiveOriginal
    {
        OriginalState& state;
        explicit ActiveOriginal(OriginalState& value) : state(value)
        {
            state.previous = g_currentOriginal;
            g_currentOriginal = &state;
        }
        ~ActiveOriginal()
        {
            state.active = false;
            g_currentOriginal = state.previous;
        }
    };

    void Dispatch(NativeHookContext* context)
    {
        HookSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (context->hookId < g_entries.size())
            {
                HookEntry* entry = g_entries[context->hookId].get();
                if (entry)
                {
                    const size_t slots = entry->parameters.size() + (entry->isStatic ? 0 : 1);
                    context->stackCount = slots > 4 ? slots - 4 : 0;
                }
                if (entry && entry->enabled && !g_shutdown.load() && !LuaEngine::Instance().IsFaulted() &&
                    g_bypassEntry != entry)
                {
                    snapshot.entry = entry;
                    snapshot.method = entry->method;
                    snapshot.original = entry->original;
                    snapshot.luaRef = entry->luaRef;
                    snapshot.tickCallback = entry->tickCallback;
                    snapshot.isStatic = entry->isStatic;
                    snapshot.parameters = entry->parameters;
                    snapshot.returnType = entry->returnType;
                }
                else if (entry)
                {
                    // Consume exactly this runtime_invoke entry; recursion in the body
                    // must enter Lua again. The caller restores any enclosing token.
                    if (g_bypassEntry == entry) g_bypassEntry = nullptr;
                    context->original = entry->original;
                    return;
                }
            }
        }
        if (!snapshot.entry)
        {
            context->original = nullptr;
            return;
        }

        context->original = snapshot.original;
        if (snapshot.luaRef == LUA_REFNIL) return;

        auto& engine = LuaEngine::Instance();
        std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
        lua_State* state = engine.GetState();
        if (!state)
        {
            context->original = snapshot.original;
            return;
        }
        const int base = lua_gettop(state);
        LuaEngine::OutputCapture outputCapture;
        if (!lua_checkstack(state, static_cast<int>(snapshot.parameters.size()) + 8)) return;
        OriginalState original;
        original.entry = snapshot.entry;
        original.method = snapshot.method;
        original.context = context;
        original.token = ++g_nextOriginalToken;
        original.parameters = snapshot.parameters;
        DispatchCleanup cleanup{&original};

        int position = 0;
        if (!snapshot.isStatic)
        {
            MonoObject* object = reinterpret_cast<MonoObject*>(ReadSlot(context, position++, false));
            original.objectHandle = MonoResolver::Instance().CreateGCHandle(object, true);
            if (!original.objectHandle)
            {
                lua_settop(state, base);
                context->original = snapshot.original;
                return;
            }
        }
        original.argumentHandles.resize(snapshot.parameters.size(), 0);
        for (size_t index = 0; index < snapshot.parameters.size(); ++index)
        {
            MonoType* type = snapshot.parameters[index];
            const uint64_t raw = ReadSlot(context, position++, IsFloat(type));
            original.arguments.push_back(raw);
            if (raw && IsReference(type))
            {
                original.argumentHandles[index] =
                    MonoResolver::Instance().CreateGCHandle(reinterpret_cast<MonoObject*>(raw), true);
                if (!original.argumentHandles[index])
                {
                    lua_settop(state, base);
                    context->original = snapshot.original;
                    return;
                }
            }
        }

        lua_rawgeti(state, LUA_REGISTRYINDEX, snapshot.luaRef);
        if (snapshot.isStatic)
            LuaBridge_PushClass(state, MonoResolver::Instance().MethodClass(snapshot.method));
        else
        {
            std::string error;
            if (!LuaBridge_TryPushInstance(
                    state, MonoResolver::Instance().GCHandleTarget(original.objectHandle), error))
            {
                ReportHookError(state, error.c_str());
                lua_settop(state, base);
                context->original = snapshot.original;
                return;
            }
        }
        lua_pushinteger(state, static_cast<lua_Integer>(original.token));
        lua_pushcclosure(state, OriginalInvoke, 1);
        for (size_t i = 0; i < snapshot.parameters.size(); ++i)
        {
            uint64_t raw = original.arguments[i];
            if (original.argumentHandles[i])
                raw = reinterpret_cast<uint64_t>(
                    MonoResolver::Instance().GCHandleTarget(original.argumentHandles[i]));
            std::string error;
            if (!LuaBridge_PushRawValue(state, snapshot.parameters[i], &raw, error))
            {
                ReportHookError(state, "[hook] failed to marshal callback arguments");
                lua_settop(state, base);
                context->original = snapshot.original;
                return;
            }
        }
        ActiveOriginal active(original);
        const int callbackStatus =
            lua_pcall(state, 2 + static_cast<int>(snapshot.parameters.size()), LUA_MULTRET, 0);
        engine.CheckHealthy();
        if (callbackStatus != LUA_OK)
        {
            ReportHookError(state, "[hook] callback failed");
            lua_settop(state, base);
            context->original = original.invoked ? nullptr : snapshot.original;
            return;
        }
        original.active = false;
        const int returnKind = MonoResolver::Instance().TypeKind(snapshot.returnType);
        if (returnKind != TYPE_VOID && lua_gettop(state) == base)
        {
            context->original = original.invoked ? nullptr : snapshot.original;
            return;
        }
        if (returnKind != TYPE_VOID && !SetResult(state, base + 1, snapshot.returnType, context))
        {
            ReportHookError(state, "[hook] callback returned an incompatible value");
            lua_settop(state, base);
            context->original = original.invoked ? nullptr : snapshot.original;
            return;
        }
        lua_settop(state, base);
        context->original = nullptr;
    }

    void DispatchAndComplete(NativeHookContext* context)
    {
        void (*tick)() = nullptr;
        MonoType* returnType = nullptr;
        const uint64_t generation = MonoRuntime::Instance().Generation();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (context->hookId < g_entries.size())
            {
                const auto* entry = g_entries[context->hookId].get();
                if (entry && entry->enabled && entry != g_bypassEntry && !g_shutdown.load())
                {
                    tick = entry->tickCallback;
                    returnType = entry->returnType;
                }
            }
        }
        Dispatch(context);
        if (context->original) NativeInvoke(context, static_cast<uint32_t>(context->stackCount));
        auto& engine = LuaEngine::Instance();
        if (!tick || engine.IsFaulted() || g_shutdown.load()) return;
        // A tick may allocate or recursively call managed code. Pin a reference return
        // across it, including the native fallback and the tick-only entry paths.
        auto& resolver = MonoResolver::Instance();
        if (generation != MonoRuntime::Instance().Generation()) return;
        const bool reference = IsReference(returnType);
        mono::ScopedGCHandle result(resolver, reference && context->resultInt
            ? resolver.CreateGCHandle(reinterpret_cast<MonoObject*>(context->resultInt), true) : 0);
        if (reference && context->resultInt && !result.value) return;
        std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
        if (!engine.GetState() || generation != MonoRuntime::Instance().Generation()) return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto* entry = context->hookId < g_entries.size() ? g_entries[context->hookId].get() : nullptr;
            if (!entry || entry->tickCallback != tick || !entry->enabled || g_shutdown.load()) return;
        }
        tick();
        if (result.value) context->resultInt = reinterpret_cast<uint64_t>(result.Target());
    }

} // namespace

extern "C" void HookDispatch(NativeHookContext* context)
{
    OriginalState* previousOriginal = g_currentOriginal;
    HookEntry* previousBypass = g_bypassEntry;
    __try
    {
        DispatchAndComplete(context);
    }
    __except (LuaEngine::Instance().HandleNativeFault())
    {
        // Dispatch's C++ frames and Lua state are no longer recoverable. Do not dereference
        // stack-owned OriginalState here; keep native hook resources resident until process exit.
        g_currentOriginal = previousOriginal;
        g_bypassEntry = previousBypass;
        if (context) context->original = nullptr;
    }
}

bool MonoHook::HookMethod(lua_State* state, MonoMethod* method, int callbackIndex, std::string& error)
{
    error.clear();
    if (g_shutdown.load())
    {
        error = "hook manager has already been shut down";
        return false;
    }
    DrainDeferred();
    if (!method || lua_type(state, callbackIndex) != LUA_TFUNCTION)
    {
        error = "hook requires a function";
        return false;
    }
    HookEntry prepared;
    if (!PrepareHook(method, prepared, error)) return false;
    const auto& parameters = prepared.parameters;
    MonoType* returnType = prepared.returnType;
    void* target = prepared.target;
    std::lock_guard<std::mutex> lock(g_mutex);
    // MinHook 的全局状态与本模块的 Hook 表必须使用同一串行化边界；否则
    // 命令线程和 Hook 回调线程同时注册方法时可能并发初始化 MinHook。
    if (!EnsureMinHook())
    {
        error = "failed to prepare native hook";
        return false;
    }
    if (g_metadataCleanupPending)
    {
        error = "metadata invalidation is still waiting for active hooks";
        return false;
    }
    auto existing = g_methods.find(method);
    if (existing != g_methods.end())
    {
        HookEntry* entry = existing->second;
        entry->parameters = parameters;
        entry->returnType = returnType;
        entry->isStatic = prepared.isStatic;
        lua_pushvalue(state, callbackIndex);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        const MH_STATUS status = MH_EnableHook(entry->target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
            error = "failed to re-enable method hook";
            return false;
        }
        QueueLuaRefLocked(entry->luaRef);
        entry->luaRef = reference;
        entry->enabled = true;
        return true;
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
    auto entry = std::make_unique<HookEntry>(std::move(prepared));
    entry->id = static_cast<uint32_t>(g_entries.size());
    lua_pushvalue(state, callbackIndex);
    entry->luaRef = luaL_ref(state, LUA_REGISTRYINDEX);
    if (!EnableNativeHook(*entry, error))
    {
        luaL_unref(state, LUA_REGISTRYINDEX, entry->luaRef);
        return false;
    }
    g_methods[method] = entry.get();
    g_entries.push_back(std::move(entry));
    return true;
}

bool MonoHook::UnhookMethod(MonoMethod* method)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto found = g_methods.find(method);
        if (found == g_methods.end()) return false;
        if (found->second->luaRef == LUA_REFNIL) return true;
        HookEntry* entry = found->second;
        QueueLuaRefLocked(entry->luaRef);
        entry->luaRef = LUA_REFNIL;
        if (!entry->tickCallback)
        {
            MH_DisableHook(entry->target);
            entry->enabled = false;
        }
    }
    // 若当前调用来自 Hook 回调，不能等待自身退出；此时只延迟 unref，
    // 下一个安全生命周期点会由 DrainDeferred() 完成回收。
    DrainDeferred();
    return true;
}

bool MonoHook::IsHooked(MonoMethod* method)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_methods.find(method);
    return found != g_methods.end() && found->second->enabled && found->second->luaRef != LUA_REFNIL;
}

bool MonoHook::InstallTick(MonoMethod* method, void (*callback)(), std::string& error)
{
    error.clear();
    if (!method || !callback)
    {
        error = "tick method is null";
        return false;
    }
    if (g_shutdown.load())
    {
        error = "hook manager has already been shut down";
        return false;
    }
    DrainDeferred();
    HookEntry prepared;
    if (!PrepareHook(method, prepared, error)) return false;
    void* target = prepared.target;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!EnsureMinHook())
    {
        error = "failed to prepare tick hook";
        return false;
    }
    if (g_metadataCleanupPending)
    {
        error = "metadata invalidation is still waiting for active hooks";
        return false;
    }

    // Commit the new tick only after its hook is enabled.
    const auto commitTick = [callback](HookEntry* next) {
        if (g_tickEntry && g_tickEntry != next)
        {
            g_tickEntry->tickCallback = nullptr;
            if (g_tickEntry->luaRef == LUA_REFNIL)
            {
                MH_DisableHook(g_tickEntry->target);
                g_tickEntry->enabled = false;
            }
        }
        next->tickCallback = callback;
        next->enabled = true;
        g_tickEntry = next;
    };
    for (auto& item : g_entries)
    {
        if (!item || item->target != target) continue;
        if (item->method != method)
        {
            error = "tick method shares its native address with another registered Mono method";
            return false;
        }
        g_methods[method] = item.get();
        const MH_STATUS status = MH_EnableHook(target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            error = "failed to enable tick hook";
            return false;
        }
        commitTick(item.get());
        return true;
    }
    auto entry = std::make_unique<HookEntry>(std::move(prepared));
    entry->id = static_cast<uint32_t>(g_entries.size());
    if (!EnableNativeHook(*entry, error))
    {
        return false;
    }
    commitTick(entry.get());
    g_methods[method] = entry.get();
    g_entries.push_back(std::move(entry));
    return true;
}

void MonoHook::UnhookAll()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry : g_entries)
        {
            if (!entry || entry->luaRef == LUA_REFNIL) continue;
            QueueLuaRefLocked(entry->luaRef);
            entry->luaRef = LUA_REFNIL;
            if (!entry->tickCallback)
            {
                MH_DisableHook(entry->target);
                entry->enabled = false;
            }
        }
    }
    DrainDeferred();
}

void MonoHook::InvalidateMetadata()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry : g_entries)
        {
            if (!entry) continue;
            if (entry->enabled) MH_DisableHook(entry->target);
            QueueLuaRefLocked(entry->luaRef);
            entry->luaRef = LUA_REFNIL;
            entry->enabled = false;
            entry->tickCallback = nullptr;
        }
        g_methods.clear();
        g_tickEntry = nullptr;
        g_metadataCleanupPending = true;
    }
    // 不能在仍有 Hook 回调时释放 trampoline 或 Lua 引用。若当前就在
    // Hook 内调用刷新，这里只标记待处理；后续命令结束或安装 Hook 时再次尝试回收。
    DrainDeferred();
}

void MonoHook::DrainDeferred()
{
    if (LuaEngine::Instance().IsFaulted()) return;
    if (HookStubsActive()) return;
    auto& engine = LuaEngine::Instance();
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    std::lock_guard<std::mutex> lock(g_mutex);
    if (HookStubsActive()) return; // recheck after waiting for the Lua mutex
    lua_State* state = engine.GetState();
    if (g_metadataCleanupPending)
    {
        for (auto& entry : g_entries)
        {
            if (!entry) continue;
            MH_RemoveHook(entry->target);
            if (entry->thunk) VirtualFree(entry->thunk, 0, MEM_RELEASE);
        }
        g_entries.clear();
        g_methods.clear();
        g_tickEntry = nullptr;
        g_metadataCleanupPending = false;
    }
    for (int reference : g_deferredLuaRefs)
        if (state) luaL_unref(state, LUA_REGISTRYINDEX, reference);
    g_deferredLuaRefs.clear();
}

void MonoHook::Shutdown()
{
    if (LuaEngine::Instance().IsFaulted()) return;
    g_shutdown.store(true);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry : g_entries)
            if (entry && entry->enabled)
            {
                MH_DisableHook(entry->target);
                entry->enabled = false;
            }
    }

    // Disable 后仍可能有线程已经进入共享跳板，必须等整个跳板（包括
    // trampoline 调用和返回路径）退出，才能删除 MinHook trampoline。
    WaitForHookStubs();
    if (LuaEngine::Instance().IsFaulted()) return;

    auto& engine = LuaEngine::Instance();
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    std::lock_guard<std::mutex> lock(g_mutex);
    lua_State* state = engine.GetState();
    for (auto& entry : g_entries)
    {
        if (!entry) continue;
        MH_RemoveHook(entry->target);
        QueueLuaRefLocked(entry->luaRef);
        entry->luaRef = LUA_REFNIL;
        if (entry->thunk) VirtualFree(entry->thunk, 0, MEM_RELEASE);
    }
    g_entries.clear();
    g_methods.clear();
    g_tickEntry = nullptr;
    g_metadataCleanupPending = false;
    for (int reference : g_deferredLuaRefs)
        if (state) luaL_unref(state, LUA_REGISTRYINDEX, reference);
    g_deferredLuaRefs.clear();
    if (g_minHookReady) MH_Uninitialize();
    g_minHookReady = false;
    g_shutdown.store(false);
}
