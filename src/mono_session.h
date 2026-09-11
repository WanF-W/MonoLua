/**
 * mono_session.h — MonoLua 运行时会话协调
 * MonoRuntime 只负责程序集快照和运行时查询；Hook、主线程调度器等
 * 需要在 metadata 变化时一起失效的组件，由本模块统一协调。这样
 * Runtime 不需要反向包含上层模块，依赖方向保持单向。
 */
#pragma once

namespace MonoSession
{
    // 刷新程序集；若 metadata generation 发生变化，同时失效 Hook 和 tick。
    bool RefreshAssemblies();
} // namespace MonoSession
