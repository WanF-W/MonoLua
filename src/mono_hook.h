/**
 * mono_hook.h — Mono JIT 方法 Hook 管理边界
 * 管理 Windows x64 Mono JIT 入口、MinHook trampoline、Lua callback 引用
 * 以及 Unity tick 内部 Hook。泛型、值类型声明类、ref/out 和结构体 ABI
 * 均在安装前拒绝；关闭时先阻止新回调，再等待在途分发结束。
 */
#pragma once

#include "common.h"
#include "mono_api.h"
struct lua_State;

namespace MonoHook
{
    bool HookMethod(lua_State* state, MonoMethod* method, int callbackIndex, std::string& error);
    bool UnhookMethod(MonoMethod* method);
    bool IsHooked(MonoMethod* method);
    // The caller owns scheduling policy; Hook only invokes the supplied tick callback.
    bool InstallTick(MonoMethod* method, void (*callback)(), std::string& error);
    bool InstallMainThreadProbe(MonoMethod* method, void (*callback)(), std::string& error);
    void UnhookAll();
    void InvalidateMetadata();
    void DrainDeferred();
    void Shutdown();
} // namespace MonoHook
