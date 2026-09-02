/**
 * ============================================================
 * mono_scheduler.cpp — Unity 主线程任务队列实现
 * ============================================================
 * schedule 只建立 registry 引用并入队；内部 tick Hook 在首次进入的游戏
 * 线程上记录主线程身份，每帧交换队列后执行，回调中新排任务留到下一帧。
 * ============================================================
 */
#include "mono_scheduler.h"
#include "mono_hook.h"
#include "lua_engine.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    std::mutex g_mutex;
    std::vector<int> g_queue;
    MonoMethod* g_tick = nullptr;
    DWORD g_mainThread = 0;
    bool g_ready = false;
}

bool MonoScheduler::Schedule(lua_State* state, int callbackIndex)
{
    if (!state || lua_type(state, callbackIndex) != LUA_TFUNCTION) return false;
    lua_pushvalue(state, callbackIndex);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.push_back(reference);
    return true;
}

bool MonoScheduler::SetTick(MonoMethod* method, std::string& error)
{
    if (!MonoHook::InstallTick(method, error)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tick = method; g_mainThread = 0; g_ready = false;
    return true;
}

MonoMethod* MonoScheduler::GetTick()
{
    std::lock_guard<std::mutex> lock(g_mutex); return g_tick;
}

bool MonoScheduler::IsReady()
{
    std::lock_guard<std::mutex> lock(g_mutex); return g_ready;
}

bool MonoScheduler::IsMainThread()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_ready && g_mainThread == GetCurrentThreadId();
}

void MonoScheduler::OnTick()
{
    const DWORD thread = GetCurrentThreadId();
    std::vector<int> pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_mainThread) g_mainThread = thread;
        if (g_mainThread != thread) return;
        g_ready = true;
        pending.swap(g_queue);
    }
    auto& engine = LuaEngine::Instance();
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    lua_State* state = engine.GetState();
    if (!state) return;
    for (int reference : pending)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
        if (lua_pcall(state, 0, 0, 0) != LUA_OK)
        {
            const char* message = lua_tostring(state, -1);
            const OutputCallback& output = engine.GetOutputCallback();
            if (output) output(message ? message : "[schedule] callback failed");
            lua_pop(state, 1);
        }
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
    }
}

void MonoScheduler::InvalidateMetadata()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tick = nullptr;
    g_mainThread = 0;
    g_ready = false;
}

void MonoScheduler::Shutdown(lua_State* state)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (state) for (int reference : g_queue) luaL_unref(state, LUA_REGISTRYINDEX, reference);
    g_queue.clear(); g_tick = nullptr; g_mainThread = 0; g_ready = false;
}
