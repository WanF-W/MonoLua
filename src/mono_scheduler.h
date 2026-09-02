/**
 * ============================================================
 * mono_scheduler.h — Unity 主线程任务调度边界
 * ============================================================
 * Lua 回调先保存到 registry 队列，再由用户指定的 Unity tick 方法执行。
 * 首次 tick 固定主线程身份，UnityEngine API 可据此拒绝工作线程调用。
 * 队列与 Lua VM 分别使用独立互斥锁，任何路径不得反向持有两把锁。
 * ============================================================
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
    bool IsMainThread();
    void OnTick();
    void InvalidateMetadata();
    void Shutdown(lua_State* state);
}
