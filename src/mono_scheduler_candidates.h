#pragma once
#include "mono_metadata.h"

namespace MonoScheduler
{
    // 调度器使用的 Unity 固定入口。这些描述只适用于内置候选，
    // 不用于公开 Lua Hook 接口传入的任意方法。
    struct Candidate
    {
        const char* klass;
        const char* method;
        int returnKind;
        bool isStatic;
    };

    inline constexpr Candidate ExecuteTasks{"UnitySynchronizationContext", "ExecuteTasks", mono_metadata::TYPE_VOID, true};
    inline constexpr Candidate DeltaTime{"Time", "get_deltaTime", mono_metadata::TYPE_R4, true};
    inline constexpr Candidate FrameCount{"Time", "get_frameCount", mono_metadata::TYPE_I4, true};
    inline constexpr Candidate ObjectName{"Object", "get_name", mono_metadata::TYPE_STRING, false};
}
