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
    bool g_probeInstalled = false; // protected by the Lua mutex
    bool g_installFailureLogged = false;
    thread_local bool g_draining = false;

    void ConfirmMainThread()
    {
        DWORD expected = 0;
        g_mainThread.compare_exchange_strong(expected, GetCurrentThreadId());
    }

    bool EnsureMainThreadProbe(std::string& error)
    {
        if (g_mainThread.load() || g_probeInstalled) return true;
        struct Candidate { const char* klass; const char* method; };
        static constexpr Candidate candidates[] = {
            {"UnitySynchronizationContext", "ExecuteTasks"}, {"Time", "get_deltaTime"}};
        auto& resolver = MonoResolver::Instance();
        std::string failures;
        for (const auto& candidate : candidates)
        {
            MonoClass* klass = MonoRuntime::Instance().FindClass("UnityEngine", candidate.klass);
            if (!klass) continue;
            for (MonoMethod* method : resolver.EnumerateMethods(klass))
            {
                const char* name = resolver.MethodName(method);
                if (!name || strcmp(name, candidate.method) != 0 ||
                    !resolver.MethodParameters(method).empty()) continue;
                std::string detail;
                if (MonoHook::InstallMainThreadProbe(method, ConfirmMainThread, detail))
                {
                    g_probeInstalled = true;
                    return true;
                }
                if (!failures.empty()) failures += "; ";
                failures += std::string(candidate.klass) + "." + candidate.method + ": " + detail;
            }
        }
        error = "main-thread probe unavailable";
        if (!failures.empty()) error += ": " + failures;
        return false;
    }
} // namespace

bool MonoScheduler::Schedule(lua_State* state, int callbackIndex)
{
    std::lock_guard<std::recursive_mutex> luaLock(LuaEngine::Instance().GetMutex());
    if (!state || lua_type(state, callbackIndex) != LUA_TFUNCTION) return false;
    lua_pushvalue(state, callbackIndex);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.size() >= MAX_SCHEDULE_QUEUE)
        {
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
            return false;
        }
        g_queue.push_back(reference);
    }
    if (!IsReady() || (!g_mainThread.load() && !g_probeInstalled))
    {
        std::string error;
        if (!AutoSetTick(error) && !g_installFailureLogged)
        {
            g_installFailureLogged = true;
            const std::string warning =
                "[schedule] " + error + "; callbacks remain queued; use mono.set_tick().\n";
            LuaEngine::Instance().EmitOutput(warning.c_str());
        }
    }
    return true;
}

bool MonoScheduler::SetTick(MonoMethod* method, std::string& error)
{
    std::lock_guard<std::recursive_mutex> luaLock(LuaEngine::Instance().GetMutex());
    if (!EnsureMainThreadProbe(error)) return false;
    if (!MonoHook::InstallTick(method, OnTick, error)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tick = method;
    g_installFailureLogged = false;
    return true;
}

bool MonoScheduler::AutoSetTick(std::string& error)
{
    std::lock_guard<std::recursive_mutex> luaLock(LuaEngine::Instance().GetMutex());
    error.clear();
    if (!EnsureMainThreadProbe(error)) return false;
    if (IsReady()) return true;
    struct Candidate
    {
        const char* klass;
        const char* method;
    };
    static constexpr Candidate candidates[] = {
        {"Time", "get_deltaTime"}, {"Time", "get_frameCount"}, {"Object", "get_name"}};
    auto& resolver = MonoResolver::Instance();
    std::string failures;
    for (const auto& candidate : candidates)
    {
        MonoClass* klass = MonoRuntime::Instance().FindClass("UnityEngine", candidate.klass);
        if (!klass) continue;
        for (MonoMethod* method : resolver.EnumerateMethods(klass))
        {
            const char* methodName = resolver.MethodName(method);
            if (!methodName || strcmp(methodName, candidate.method) != 0 ||
                !resolver.MethodParameters(method).empty())
                continue;
            if (SetTick(method, error)) return true;
            if (!failures.empty()) failures += "; ";
            failures += std::string(candidate.klass) + "." + candidate.method + ": " + error;
        }
    }
    error = "no usable Unity tick method was found";
    if (!failures.empty()) error += ": " + failures;
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
    g_installFailureLogged = false;
}

void MonoScheduler::Shutdown()
{
    InvalidateMetadata();
}
