#pragma once
// Private registry shared by installation and dispatch. Lock order: Lua, then registry.
#include "mono_hook_metadata.h"
#include "../minhook_src/MinHook.h"
#include <memory>
namespace mono_hook_detail
{
    inline int MinHookFaultFilter(EXCEPTION_POINTERS* info) noexcept
    {
        if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
                   ? EXCEPTION_EXECUTE_HANDLER
                   : EXCEPTION_CONTINUE_SEARCH;
    }

    template <typename Function, typename... Arguments>
    MH_STATUS SafeMinHookCallSeh(Function function, Arguments... arguments)
    {
        __try
        {
            return function(arguments...);
        }
        __except (MinHookFaultFilter(GetExceptionInformation()))
        {
            bridge_lifecycle::g_nativeCallFaulted = true;
            bridge_lifecycle::g_nativeCallFaultCode = GetExceptionCode();
            return MH_UNKNOWN;
        }
    }

    template <typename Function, typename... Arguments>
    MH_STATUS SafeMinHookCall(Function function, Arguments... arguments)
    {
        try
        {
            return SafeMinHookCallSeh(function, arguments...);
        }
        catch (...)
        {
            bridge_lifecycle::g_nativeCallFaulted = true;
            bridge_lifecycle::g_nativeCallFaultCode = 0;
            return MH_UNKNOWN;
        }
    }

    extern std::mutex g_mutex;
    extern std::vector<std::unique_ptr<HookEntry>> g_entries;
    extern std::map<MonoMethod*, HookEntry*> g_methods;
    extern bool g_minHookReady;
    extern std::atomic<bool> g_shutdown;
    extern std::vector<int> g_deferredLuaRefs;
    extern bool g_metadataCleanupPending;
    extern HookEntry* g_tickEntry;
    extern HookEntry* g_probeEntry;
    bool HookStubsActive();
}
