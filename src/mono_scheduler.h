/**
 * mono_scheduler.h — Unity 主线程任务调度边界
 * Lua 回调先保存到 registry 队列，再由用户指定的 Unity tick 方法执行。
 * 首次 tick 固定排空任务的线程，避免同一 tick 在其他线程上执行队列。
 * 队列与 Lua VM 分别使用独立互斥锁，任何路径不得反向持有两把锁。
 */
#pragma once

#include "common.h"
#include "mono_api.h"

struct lua_State;

namespace MonoScheduler
{
    bool Schedule(lua_State* state, int callbackIndex);
    bool SetTick(MonoMethod* method, std::string& error);
    bool AutoSetTick(std::string& error);
    MonoMethod* GetTick();
    bool IsReady();
    void OnTick();
    void InvalidateMetadata();
    void Shutdown();
} // namespace MonoScheduler
