/**
 * hook_stub.h — C++ 与 x64 MASM Hook 跳板 ABI 边界
 * 该入口只保存 Windows x64 易失参数槽，不解释 Mono 类型；参数类型解释、
 * 不支持 ABI 的拒绝和回跳策略全部由 mono_hook.cpp 负责。
 */
#pragma once
#include <cstdint>

#if !defined(_M_X64)
#error HookDetourEntry is implemented only for the Windows x64 ABI.
#endif

extern "C" unsigned char HookDetourEntry[];
struct NativeHookContext;
extern "C" void NativeInvoke(NativeHookContext* context, uint32_t stackCount);

// 业务代码只取得入口地址，不会按 C++ 调用约定直接调用该汇编跳板。
inline void* GetHookDetourAddress() noexcept
{
    return static_cast<void*>(HookDetourEntry);
}
