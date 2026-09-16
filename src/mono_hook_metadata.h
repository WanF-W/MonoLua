#pragma once
#include "mono_resolver.h"
#include "mono_scheduler_candidates.h"
extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}
namespace mono_hook_detail
{
    struct HookEntry
    {
        MonoMethod* method = nullptr;
        void* target = nullptr;
        void* thunk = nullptr;
        void* original = nullptr;
        uint32_t id = 0;
        int luaRef = LUA_REFNIL;
        bool enabled = false;
        void (*probeCallback)() = nullptr;
        void (*tickCallback)() = nullptr;
        bool isStatic = false;
        std::vector<MonoType*> parameters;
        MonoType* returnType = nullptr;
    };

    bool PrepareHook(MonoMethod* method, HookEntry& entry, std::string& error,
                     const MonoScheduler::Candidate* candidate = nullptr);
}
