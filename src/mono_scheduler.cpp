/**
 * mono_scheduler.cpp — Unity 主线程任务队列实现
 * 独立的一次性探针确认线程；tick 只在已确认的线程排空队列。
 * 回调中新排任务留到下一次非嵌套 tick。
 */
#include "mono_scheduler.h"
#include "mono_hook.h"
#include "mono_runtime.h"
#include "mono_resolver.h"
#include "lua_engine.h"
#include "mono_feature_fault.h"
#include "mono_scheduler_candidates.h"

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
    bool g_probeInstalled = false; // 由 Lua 互斥锁保护
    bool g_diagnosticsReady = false; // 由 Lua 互斥锁保护
    bool g_failureLogged = false; // 由 Lua 互斥锁保护
    std::string g_pendingDiagnostics; // 由 Lua 互斥锁保护
    thread_local bool g_draining = false;

    void ConfirmMainThread()
    {
        DWORD expected = 0;
        g_mainThread.compare_exchange_strong(expected, GetCurrentThreadId());
    }

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
        MonoFeatureFault fault;
        __try
        {
            return FindSchedulerEntryImpl(candidate, error, fault);
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        fault.Describe(fault.stage, error);
        return nullptr;
    }

    const MonoScheduler::Candidate* FindFixedCandidate(MonoMethod* method)
    {
        if (!method) return nullptr;
        for (const auto* candidate : {&MonoScheduler::ExecuteTasks, &MonoScheduler::DeltaTime,
                                      &MonoScheduler::FrameCount, &MonoScheduler::ObjectName})
        {
            std::string detail;
            if (FindSchedulerEntry(*candidate, detail) == method) return candidate;
        }
        return nullptr;
    }

    bool EnsureMainThreadProbe(std::string& error)
    {
        if (g_mainThread.load() || g_probeInstalled) return true;
        error.clear();
        for (const auto* candidate : {&MonoScheduler::ExecuteTasks, &MonoScheduler::DeltaTime})
        {
            std::string detail;
            MonoMethod* method = FindSchedulerEntry(*candidate, detail);
            const bool installed = method &&
                MonoHook::InstallSchedulerEntry(method, *candidate, ConfirmMainThread, true, detail);
            ReportCandidate(*candidate, "probe", installed, detail);
            if (installed)
            {
                g_probeInstalled = true;
                error.clear();
                return true;
            }
            if (!error.empty()) error += "; ";
            error += std::string(candidate->klass) + "." + candidate->method + ": " + detail;
        }
        return false;
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
    // 调度器暂时不可用时仍保留任务，
    // 后续可以通过 set_tick 或下一次自动尝试恢复。
    if (!IsReady() || (!g_mainThread.load() && !g_probeInstalled))
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
    if (!EnsureMainThreadProbe(error)) return false;
    // 手动指定内置 Unity 入口时也走固定候选路径，避免再次触发泛型反射查询。
    if (const auto* candidate = FindFixedCandidate(method))
    {
        if (!MonoHook::InstallSchedulerEntry(method, *candidate, OnTick, false, error)) return false;
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
    if (!EnsureMainThreadProbe(error))
    {
        ReportSchedulerFailure(error);
        return false;
    }

    std::string failures;
    for (const auto* candidate : {&DeltaTime, &FrameCount, &ObjectName})
    {
        std::string detail;
        MonoMethod* method = FindSchedulerEntry(*candidate, detail);
        const bool installed = method && MonoHook::InstallSchedulerEntry(method, *candidate, OnTick, false, detail);
        ReportCandidate(*candidate, "tick", installed, detail);
        if (installed)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_tick = method;
            g_failureLogged = false;
            return true;
        }
        if (!failures.empty()) failures += "; ";
        failures += std::string(candidate->klass) + "." + candidate->method + ": " + detail;
    }
    error = "no usable Unity tick: " + failures;
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
    g_probeInstalled = false;
    g_pendingDiagnostics.clear();
    g_failureLogged = false;
}

void MonoScheduler::Shutdown()
{
    InvalidateMetadata();
    std::lock_guard<std::recursive_mutex> luaLock(LuaEngine::Instance().GetMutex());
    g_diagnosticsReady = false;
}
