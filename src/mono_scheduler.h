/**
 * mono_scheduler.h — Unity 主线程任务调度边界
 * Lua 回调先保存到 registry 队列，再由用户指定的 Unity tick 方法执行。
 * 独立探针确认执行线程；切换 tick 保留线程身份，IsReady 只表示已安装 tick。
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
