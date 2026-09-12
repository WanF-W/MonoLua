/**
 * common.h — MonoLua 公共基础依赖
 * 只放置多个核心模块共同需要的 Windows/标准库声明和通用宏。
 * Mono API 类型集中在 mono_api.h，避免本文件变成运行时声明集合。
 */
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace bridge_lifecycle
{
    // Tool-initiated managed calls must never establish the Unity thread identity.
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
    // Process detach must not wait for threads that Windows may already have terminated.
    inline std::atomic<bool> g_processTerminating{false};
    inline std::atomic<bool> g_sessionFaulted{false};
} // namespace bridge_lifecycle
