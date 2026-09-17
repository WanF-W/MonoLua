// Mono JIT Hook：thunk 标识方法，共享汇编保存参数，NativeInvoke 使用独立栈。
// original() 经 runtime_invoke 和线程局部旁路；已尝试原调用的回调不自动重试。
// Lua 锁在 Hook 表锁之前。原生回退在途期间同样保留 trampoline。
#include "mono_hook.h"
#include "mono_hook_internal.h"
using namespace mono_hook_detail;
#include "hook_stub.h"
#include "mono_metadata.h"
#include "mono_resolver.h"
#include "mono_runtime.h"
#include "lua_binding_internal.h"
#include "lua_engine.h"
#include "lua_value_internal.h"
#include "mono_handle.h"
#include "mono_feature_fault.h"
#include "mono_scheduler_candidates.h"
#include "../minhook_src/MinHook.h"
#include <exception>
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

namespace mono_hook_detail
{
    using namespace mono_metadata;

    struct OriginalState
    {
        HookEntry* entry = nullptr;
        MonoMethod* method = nullptr;
        NativeHookContext* context = nullptr;
        uint64_t token = 0;
        bool invoked = false;
        MonoGCHandle resultHandle = 0;
        std::vector<MonoType*> parameters;
        MonoGCHandle objectHandle = 0;
        std::vector<uint64_t> arguments;
        std::vector<MonoGCHandle> argumentHandles;
        bool active = true;
        OriginalState* previous = nullptr;
    };

    thread_local unsigned g_dispatchDepth = 0;
    thread_local HookEntry* g_bypassEntry = nullptr;
    thread_local OriginalState* g_currentOriginal = nullptr;
    uint64_t g_nextOriginalToken = 0; // 由 Lua 互斥锁保护

    int HookExceptionFilter(EXCEPTION_POINTERS* info) noexcept
    {
        // 原生 Hook 回调可能已经持有 Lua 互斥锁。即使故障来自命令线程，
        // 也不能把它当作可恢复异常，否则 SEH 会跳过锁析构并造成永久死锁。
        return LuaEngine::Instance().HandleNativeFault(info);
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
        for (MonoGCHandle handle : original.argumentHandles)
            if (handle) resolver.FreeGCHandle(handle);
        original.argumentHandles.clear();
        if (original.objectHandle) resolver.FreeGCHandle(original.objectHandle);
        original.objectHandle = 0;
        resolver.FreeGCHandle(original.resultHandle);
        original.resultHandle = 0;
    }

    bool ReadSlotSafely(const NativeHookContext* context, int position, bool floating, uint64_t& value)
    {
        MonoFeatureFault fault;
        fault.stage = "hook argument read";
        fault.catchAllMemoryAccess = true;
        __try
        {
            if (!context) return false;
            if (position < 4)
            {
                const uint64_t* registers = floating ? &context->xmm0 : &context->rcx;
                value = registers[position];
            }
            else
                value = reinterpret_cast<const uint64_t*>(context->stackArgs)[position - 4];
            return true;
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (fault.code)
        {
            char message[160]{};
            sprintf_s(message, "[MonoLua][Hook] %s: native exception 0x%08lX at %p\n",
                      fault.stage, fault.code, fault.address);
            OutputDebugStringA(message);
        }
        return false;
    }

    bool InvokeOriginalSafely(NativeHookContext* context)
    {
        MonoFeatureFault fault;
        fault.stage = "hook original invocation";
        fault.catchAllMemoryAccess = true;
        __try
        {
            NativeInvoke(context, static_cast<uint32_t>(context ? context->stackCount : 0));
            return true;
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (fault.code)
        {
            char message[160]{};
            sprintf_s(message, "[MonoLua][Hook] %s: native exception 0x%08lX at %p\n",
                      fault.stage, fault.code, fault.address);
            OutputDebugStringA(message);
        }
        return false;
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
        // InvokeMethod 同时保留了已经完成的原始结果和 GC 根。
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
        bridge_lifecycle::ClearNativeCallFault();
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
                    // 只旁路当前这一次 runtime_invoke；方法体递归时仍需重新进入 Lua，
                    // 外层调用方会恢复原有旁路标记。
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
            uint64_t raw = 0;
            if (!ReadSlotSafely(context, position++, false, raw))
            {
                context->original = snapshot.original;
                return;
            }
            MonoObject* object = reinterpret_cast<MonoObject*>(raw);
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
            uint64_t raw = 0;
            if (!ReadSlotSafely(context, position++, IsFloat(type), raw))
            {
                lua_settop(state, base);
                context->original = snapshot.original;
                return;
            }
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
        if (!context) return;
        void (*tick)() = nullptr;
        MonoType* returnType = nullptr;
        const uint64_t generation = MonoRuntime::Instance().Generation();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (context->hookId < g_entries.size())
            {
                auto* entry = g_entries[context->hookId].get();
                if (entry && entry->enabled && entry != g_bypassEntry && !g_shutdown.load())
                {
                    if (g_dispatchDepth == 1 && bridge_lifecycle::g_managedCallDepth == 0)
                        tick = entry->tickCallback;
                    returnType = entry->returnType;
                }
            }
        }
        Dispatch(context);
        if (context->original && !InvokeOriginalSafely(context)) context->original = nullptr;
        auto& engine = LuaEngine::Instance();
        if (!tick || engine.IsFaulted() || g_shutdown.load()) return;
        // tick 可能分配对象或递归调用托管代码。整个过程固定引用返回值，
        // 也覆盖原生回退和只有 tick 的入口。
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

    void RestoreHookStateAndCallOriginal(NativeHookContext* context, OriginalState* previousOriginal,
                                         HookEntry* previousBypass) noexcept
    {
        // HookDispatch 的异常路径不能再依赖 Lua 或容器分配。若 Dispatch
        // 已经写入 trampoline，尽力执行一次原方法，避免把未定义返回值交给游戏。
        void* original = nullptr;
        __try
        {
            original = context ? context->original : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            original = nullptr;
        }
        if (original)
        {
            InvokeOriginalSafely(context);
            __try
            {
                if (context) context->original = nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        // SEH 会跳过 ActiveOriginal 析构；会话隔离后不能留下指向已展开
        // 栈帧的悬空 OriginalState。
        g_currentOriginal = LuaEngine::Instance().IsFaulted() ? nullptr : previousOriginal;
        g_bypassEntry = previousBypass;
    }

    void EmitHookDispatchFailureSeh(const char* message)
    {
        __try
        {
            LuaEngine::Instance().EmitOutput(message);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // 错误报告本身不能再次越过 Hook 边界。
        }
    }

    void ReportHookDispatchFailure(const char* message) noexcept
    {
        if (!message) return;
        OutputDebugStringA(message);
        try
        {
            EmitHookDispatchFailureSeh(message);
        }
        catch (...)
        {
            // 日志本身失败不能让异常从原生 Hook 回调边界逃逸。
        }
    }

    void DispatchSehOnly(NativeHookContext* context, OriginalState* previousOriginal,
                         HookEntry* previousBypass)
    {
        __try
        {
            DispatchAndComplete(context);
        }
        __except (HookExceptionFilter(GetExceptionInformation()))
        {
            // Dispatch 的 C++ 帧和 Lua 状态已经不能恢复。这里不能解引用栈上的
            // OriginalState；原生 Hook 资源保留到进程结束。
            RestoreHookStateAndCallOriginal(context, previousOriginal, previousBypass);
        }
    }

} // namespace

extern "C" void HookDispatch(NativeHookContext* context)
{
    const unsigned previousDepth = g_dispatchDepth;
    const unsigned previousManagedDepth = bridge_lifecycle::g_managedCallDepth;
    ++g_dispatchDepth;
    OriginalState* previousOriginal = g_currentOriginal;
    HookEntry* previousBypass = g_bypassEntry;
    try
    {
        DispatchSehOnly(context, previousOriginal, previousBypass);
    }
    catch (const std::exception& exception)
    {
        OutputDebugStringA("[MonoLua] hook C++ exception: ");
        const char* detail = exception.what();
        OutputDebugStringA(detail ? detail : "unknown C++ exception");
        OutputDebugStringA("\n");
        char message[512]{};
        sprintf_s(message, "[hook] callback failed: %s\n",
                  detail ? detail : "unknown C++ exception");
        ReportHookDispatchFailure(message);
        RestoreHookStateAndCallOriginal(context, previousOriginal, previousBypass);
    }
    catch (...)
    {
        ReportHookDispatchFailure("[MonoLua] hook unknown C++ exception\n[hook] callback failed: unknown C++ exception\n");
        RestoreHookStateAndCallOriginal(context, previousOriginal, previousBypass);
    }
    g_dispatchDepth = previousDepth;
    bridge_lifecycle::g_managedCallDepth = previousManagedDepth;
}
