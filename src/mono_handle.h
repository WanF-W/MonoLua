// Temporary Mono roots. Ownership can be transferred to Lua userdata explicitly.
#pragma once
#include "mono_resolver.h"

namespace mono
{
    struct ScopedGCHandle
    {
        MonoResolver& resolver;
        uint32_t value;

        ScopedGCHandle(MonoResolver& owner, uint32_t handle) : resolver(owner), value(handle) {}
        ScopedGCHandle(const ScopedGCHandle&) = delete;
        ScopedGCHandle& operator=(const ScopedGCHandle&) = delete;
        ~ScopedGCHandle()
        {
            if (!bridge_lifecycle::g_sessionFaulted.load()) resolver.FreeGCHandle(value);
        }

        MonoObject* Target() const { return resolver.GCHandleTarget(value); }
    };

    struct GCHandles
    {
        std::vector<uint32_t> values;

        explicit GCHandles(size_t count = 0) : values(count, 0) {}
        GCHandles(const GCHandles&) = delete;
        GCHandles& operator=(const GCHandles&) = delete;
        ~GCHandles()
        {
            if (bridge_lifecycle::g_sessionFaulted.load()) return;
            for (uint32_t handle : values)
                MonoResolver::Instance().FreeGCHandle(handle);
        }
    };
} // namespace mono
