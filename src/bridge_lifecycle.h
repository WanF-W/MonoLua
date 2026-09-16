#pragma once
#include <windows.h>
#include <atomic>

namespace bridge_lifecycle
{
    // 只记录静态阶段名，不分配内存；SEH 过滤器在展开栈之前读取。
    inline thread_local const char* g_nativeStage = "native execution";
    inline thread_local bool g_nativeCallFaulted = false;
    inline thread_local DWORD g_nativeCallFaultCode = 0;
    inline void ClearNativeCallFault() noexcept
    {
        g_nativeCallFaulted = false;
        g_nativeCallFaultCode = 0;
    }
    struct NativeStageScope
    {
        const char* previous = g_nativeStage;
        explicit NativeStageScope(const char* stage) { g_nativeStage = stage; }
        ~NativeStageScope() { g_nativeStage = previous; }
        NativeStageScope(const NativeStageScope&) = delete;
        NativeStageScope& operator=(const NativeStageScope&) = delete;
    };
    // 工具主动发起的托管调用不能建立 Unity 主线程身份。
    inline thread_local unsigned g_managedCallDepth = 0;
    struct ManagedCallScope
    {
        const unsigned previous = g_managedCallDepth;
        ManagedCallScope() { ++g_managedCallDepth; }
        ~ManagedCallScope() { g_managedCallDepth = previous; }
        ManagedCallScope(const ManagedCallScope&) = delete;
        ManagedCallScope& operator=(const ManagedCallScope&) = delete;
    };
    inline constexpr DWORD SESSION_FAULT_CODE = 0xE04D4C01;
    // 进程分离时不能等待 Windows 已经终止的线程。
    inline std::atomic<bool> g_processTerminating{false};
    inline std::atomic<bool> g_sessionFaulted{false};
} // namespace bridge_lifecycle
