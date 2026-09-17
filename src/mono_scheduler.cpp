/**
 * mono_scheduler.cpp — Unity 主线程任务队列实现
 * 首次自然 tick 确认线程，后续只在该线程排空队列。
 * 回调中新排任务留到下一次非嵌套 tick。
 */
#include "mono_scheduler.h"
#include "mono_hook.h"
#include "mono_runtime.h"
#include "mono_resolver.h"
#include "lua_engine.h"
#include "mono_feature_fault.h"
#include "mono_scheduler_candidates.h"
#include <exception>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    constexpr size_t MAX_SCHEDULE_QUEUE = 1024;
    std::mutex g_mutex;
    std::vector<int> g_queue;
    MonoMethod* g_tick = nullptr;
    std::atomic<DWORD> g_mainThread{0};
    bool g_diagnosticsReady = false; // 由 Lua 互斥锁保护
    bool g_failureLogged = false; // 由 Lua 互斥锁保护
    std::string g_pendingDiagnostics; // 由 Lua 互斥锁保护
    thread_local bool g_draining = false;

    void ReportCandidate(const MonoScheduler::Candidate& candidate, const char* role,
                         bool installed, const std::string& detail)
    {
        if (installed) return;
        const std::string message = std::string("[scheduler] ") + role + " " + candidate.klass + "." +
            candidate.method + ": " + detail + '\n';
        OutputDebugStringA(message.c_str());
        if (g_diagnosticsReady)
            LuaEngine::Instance().EmitOutput(message.c_str());
        else
            g_pendingDiagnostics += message;
    }

    void ReportSchedulerFailure(const std::string& error)
    {
        if (g_failureLogged) return;
        g_failureLogged = true;
        const std::string message = "[warning] main-thread scheduler unavailable: " + error + '\n';
        OutputDebugStringA(message.c_str());
        if (g_diagnosticsReady)
            LuaEngine::Instance().EmitOutput(message.c_str());
        else
            g_pendingDiagnostics += message;
    }

    MonoMethod* FindSchedulerEntryImpl(const MonoScheduler::Candidate& candidate, std::string& error,
                                      MonoFeatureFault& fault)
    {
        auto& resolver = MonoResolver::Instance();
        fault.stage = "scheduler discovery: find class";
        MonoClass* klass = MonoRuntime::Instance().FindClass("UnityEngine", candidate.klass);
        if (!klass)
        {
            error = "class not found";
            return nullptr;
        }
        fault.stage = "scheduler discovery: enumerate methods";
        for (MonoMethod* method : resolver.EnumerateMethods(klass))
        {
            fault.stage = "scheduler discovery: method name";
            const char* name = resolver.MethodName(method);
            if (!name || strcmp(name, candidate.method) != 0) continue;
            fault.stage = "scheduler discovery: parameter signature";
            if (resolver.MethodParameters(method).empty()) return method;
        }
        error = "parameterless method not found";
        return nullptr;
    }

    MonoMethod* FindSchedulerEntry(const MonoScheduler::Candidate& candidate, std::string& error)
    {
        bridge_lifecycle::ClearNativeCallFault();
        MonoFeatureFault fault;
        MonoMethod* result = nullptr;
        __try
        {
            result = FindSchedulerEntryImpl(candidate, error, fault);
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (ConsumeNativeCallFault(fault.stage, error)) return nullptr;
        if (result && !fault.code) return result;
        if (fault.code)
            fault.Describe(fault.stage, error);
        else if (error.empty())
            error = "scheduler discovery failed";
        return nullptr;
    }

    const MonoScheduler::Candidate* FindFixedCandidate(MonoMethod* method)
    {
        if (!method) return nullptr;
        std::string detail;
        return FindSchedulerEntry(MonoScheduler::DeltaTime, detail) == method
            ? &MonoScheduler::DeltaTime : nullptr;
    }

} // namespace

bool MonoScheduler::Schedule(lua_State* state, int callbackIndex)
{
    std::lock_guard<std::recursive_mutex> luaLock(LuaEngine::Instance().GetMutex());
    if (!state || lua_type(state, callbackIndex) != LUA_TFUNCTION) return false;
    lua_pushvalue(state, callbackIndex);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.size() < MAX_SCHEDULE_QUEUE)
        {
            g_queue.push_back(reference);
            queued = true;
        }
    }
    if (!queued)
    {
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
        return false;
    }
    // 调度器暂时不可用时仍保留任务，后续可以通过 set_tick 重试。
    if (!IsReady())
    {
        std::string error;
        AutoSetTick(error);
    }
    return true;
}

bool MonoScheduler::SetTick(MonoMethod* method, std::string& error)
{
    std::lock_guard<std::recursive_mutex> luaLock(LuaEngine::Instance().GetMutex());
    error.clear();
    // 手动指定默认 Unity tick 时也走固定路径，避免再次触发泛型反射查询。
    if (const auto* candidate = FindFixedCandidate(method))
    {
        if (!MonoHook::InstallSchedulerEntry(method, *candidate, OnTick, error)) return false;
    }
    else if (!MonoHook::InstallTick(method, OnTick, error)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tick = method;
    g_failureLogged = false;
    return true;
}

bool MonoScheduler::AutoSetTick(std::string& error)
{
    std::lock_guard<std::recursive_mutex> luaLock(LuaEngine::Instance().GetMutex());
    error.clear();
    if (GetTick()) return true;

    std::string detail;
    MonoMethod* method = FindSchedulerEntry(DeltaTime, detail);
    const bool installed = method && MonoHook::InstallSchedulerEntry(method, DeltaTime, OnTick, detail);
    ReportCandidate(DeltaTime, "tick", installed, detail);
    if (installed)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tick = method;
        g_failureLogged = false;
        return true;
    }
    error = "Time.get_deltaTime: " + detail;
    ReportSchedulerFailure(error);
    return false;
}

MonoMethod* MonoScheduler::GetTick()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_tick;
}

bool MonoScheduler::IsReady()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_tick != nullptr;
}

bool MonoScheduler::IsMainThread()
{
    const DWORD thread = g_mainThread.load();
    return thread != 0 && thread == GetCurrentThreadId();
}

void MonoScheduler::FlushDiagnostics()
{
    auto& engine = LuaEngine::Instance();
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    g_diagnosticsReady = true;
    if (g_pendingDiagnostics.empty()) return;
    engine.EmitOutput(g_pendingDiagnostics.c_str());
    g_pendingDiagnostics.clear();
}

void MonoScheduler::OnTick()
{
    if (g_draining) return;
    // Hook dispatch excludes nested hooks and MonoLua-initiated managed calls.
    // Custom ticks must be naturally called on the Unity main thread.
    DWORD expected = 0;
    g_mainThread.compare_exchange_strong(expected, GetCurrentThreadId());
    const DWORD mainThread = g_mainThread.load();
    if (!mainThread || mainThread != GetCurrentThreadId()) return;
    auto& engine = LuaEngine::Instance();
    if (engine.IsFaulted()) return;
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    lua_State* state = engine.GetState();
    if (!state) return;
    g_draining = true;
    struct DrainGuard
    {
        ~DrainGuard() { g_draining = false; }
    } guard;
    const DWORD thread = GetCurrentThreadId();
    std::vector<int> pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_mainThread.load() != thread) return;
        pending.swap(g_queue);
    }
    for (int reference : pending)
    {
        const int base = lua_gettop(state);
        try
        {
            LuaEngine::OutputCapture outputCapture;
            lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
            const int status = lua_pcall(state, 0, 0, 0);
            engine.CheckHealthy();
            if (status != LUA_OK)
            {
                const std::string message = LuaEngine::ErrorText(state, -1);
                const std::string error =
                    std::string("[schedule] ") + message + '\n';
                engine.EmitOutput(error.c_str());
                lua_pop(state, 1);
            }
        }
        catch (const std::exception& exception)
        {
            char message[320]{};
            sprintf_s(message, "[MonoLua][Scheduler] callback failed: %s\n",
                      exception.what() ? exception.what() : "unknown C++ exception");
            OutputDebugStringA(message);
        }
        catch (...)
        {
            OutputDebugStringA("[MonoLua][Scheduler] callback failed: unknown C++ exception\n");
        }
        lua_settop(state, base);
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
    }
}

void MonoScheduler::InvalidateMetadata()
{
    // 程序集快照已经失效，不能继续使用旧 tick；尚未交换出去的回调也不能
    // 在没有新 tick 的情况下无限保留 registry 引用。调用方通常已经持有
    // LuaEngine 锁，这里使用可重入锁兼容直接从 Lua API 触发的刷新。
    auto& engine = LuaEngine::Instance();
    if (engine.IsFaulted()) return;
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    std::lock_guard<std::mutex> lock(g_mutex);
    if (lua_State* state = engine.GetState())
        for (int reference : g_queue)
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
    g_queue.clear();
    g_tick = nullptr;
    g_mainThread = 0;
    g_pendingDiagnostics.clear();
    g_failureLogged = false;
}

void MonoScheduler::Shutdown()
{
    InvalidateMetadata();
    std::lock_guard<std::recursive_mutex> luaLock(LuaEngine::Instance().GetMutex());
    g_diagnosticsReady = false;
}
