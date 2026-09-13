#pragma once
#include "common.h"
#include <cstdio>

// 调度器候选查找、元数据准备和原生 Hook 安装使用这条局部边界。
// 功能级错误不能调用 LuaEngine::Abort，也不能关闭通信管道。
struct MonoFeatureFault
{
    DWORD code = 0;
    void* address = nullptr;
    const char* stage = "metadata inspection";
    bool catchAllMemoryAccess = false;

    int Filter(EXCEPTION_POINTERS* info) noexcept
    {
        if (bridge_lifecycle::g_sessionFaulted.load() || !info || !info->ExceptionRecord)
            return EXCEPTION_CONTINUE_SEARCH;
        const auto* record = info->ExceptionRecord;
        // 普通元数据查询只恢复读取异常；调度器安装还要把 MinHook
        // 改写目标入口时的读写异常局部化。栈损坏、C++ 异常和会话故障信号
        // 仍然交给外层故障边界处理。
        if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
            record->ExceptionCode != EXCEPTION_IN_PAGE_ERROR)
            return EXCEPTION_CONTINUE_SEARCH;
        if (!catchAllMemoryAccess &&
            (record->NumberParameters < 2 || record->ExceptionInformation[0] != 0))
            return EXCEPTION_CONTINUE_SEARCH;
        code = record->ExceptionCode;
        address = record->ExceptionAddress;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void Describe(const char* phase, std::string& error) const
    {
        char message[256]{};
        sprintf_s(message, "%s: native exception 0x%08lX at %p",
                  phase, code, address);
        error = message;
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }
};
