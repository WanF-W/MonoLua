// 临时 Mono 根；所有权可以显式转交给 Lua userdata。
#pragma once
#include "mono_resolver.h"

namespace mono
{
    struct ScopedGCHandle
    {
        MonoResolver& resolver;
        MonoGCHandle value;

        ScopedGCHandle(MonoResolver& owner, MonoGCHandle handle) : resolver(owner), value(handle) {}
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
        std::vector<MonoGCHandle> values;

        explicit GCHandles(size_t count = 0) : values(count, 0) {}
        GCHandles(const GCHandles&) = delete;
        GCHandles& operator=(const GCHandles&) = delete;
        ~GCHandles()
        {
            if (bridge_lifecycle::g_sessionFaulted.load()) return;
            for (MonoGCHandle handle : values)
                MonoResolver::Instance().FreeGCHandle(handle);
        }
    };
} // namespace mono
