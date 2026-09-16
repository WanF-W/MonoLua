#pragma once
#include "mono_resolver.h"

namespace mono_native
{
    inline int AccessFaultFilter(EXCEPTION_POINTERS* info) noexcept
    {
        if (!info || !info->ExceptionRecord)
            return EXCEPTION_CONTINUE_SEARCH;
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
                   ? EXCEPTION_EXECUTE_HANDLER
                   : EXCEPTION_CONTINUE_SEARCH;
    }

    // 所有通过导出表调用 Mono 的薄包装都经过同一个最小 SEH 边界。
    // 这里不持有 C++ 资源，异常只会转换成默认返回值，调用方仍可正常
    // 展开自己的锁和临时对象，把错误限制在当前功能。
    template <typename Function, typename... Arguments>
    using MonoCallResult = std::invoke_result_t<Function, Arguments...>;

    template <typename Function, typename... Arguments>
    MonoCallResult<Function, Arguments...> SafeMonoCallSeh(Function function, bool& failed,
                                                            Arguments... arguments)
    {
        using Result = MonoCallResult<Function, Arguments...>;
        __try
        {
            return function(arguments...);
        }
        __except (AccessFaultFilter(GetExceptionInformation()))
        {
            failed = true;
            bridge_lifecycle::g_nativeCallFaulted = true;
            bridge_lifecycle::g_nativeCallFaultCode = GetExceptionCode();
            return Result{};
        }
    }

    template <typename Function, typename... Arguments>
    MonoCallResult<Function, Arguments...> SafeMonoCall(Function function, bool& failed,
                                                         Arguments... arguments)
    {
        using Result = MonoCallResult<Function, Arguments...>;
        try
        {
            return SafeMonoCallSeh(function, failed, arguments...);
        }
        catch (...)
        {
            failed = true;
            bridge_lifecycle::g_nativeCallFaulted = true;
            bridge_lifecycle::g_nativeCallFaultCode = 0;
            return Result{};
        }
    }

    template <typename Function, typename... Arguments>
    bool SafeMonoCallVoidSeh(Function function, Arguments... arguments)
    {
        __try
        {
            function(arguments...);
            return true;
        }
        __except (AccessFaultFilter(GetExceptionInformation()))
        {
            bridge_lifecycle::g_nativeCallFaulted = true;
            bridge_lifecycle::g_nativeCallFaultCode = GetExceptionCode();
            return false;
        }
    }

    template <typename Function, typename... Arguments>
    bool SafeMonoCallVoid(Function function, Arguments... arguments)
    {
        try
        {
            return SafeMonoCallVoidSeh(function, arguments...);
        }
        catch (...)
        {
            bridge_lifecycle::g_nativeCallFaulted = true;
            bridge_lifecycle::g_nativeCallFaultCode = 0;
            return false;
        }
    }

}
