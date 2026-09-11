/**
 * mono_session.cpp — MonoLua 运行时会话协调实现
 * metadata 变化是一个跨模块生命周期事件：Runtime 更新快照，Hook 和
 * Scheduler 必须同时失效。集中处理这个事件，避免 Runtime 直接依赖
 * 上层组件，也避免每个 Lua 入口各自复制失效顺序。
 */
#include "mono_session.h"

#include "mono_hook.h"
#include "mono_runtime.h"
#include "mono_scheduler.h"

bool MonoSession::RefreshAssemblies()
{
    auto& runtime = MonoRuntime::Instance();
    if (!runtime.RefreshAssemblies()) return false;
    if (runtime.TakeMetadataChanged())
    {
        MonoHook::InvalidateMetadata();
        MonoScheduler::InvalidateMetadata();
    }
    return true;
}
