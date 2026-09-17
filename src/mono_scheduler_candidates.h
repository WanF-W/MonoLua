#pragma once
#include "mono_metadata.h"

namespace MonoScheduler
{
    // 调度器使用的 Unity 固定入口；不用于公开 Lua Hook 接口传入的任意方法。
    struct Candidate
    {
        const char* klass;
        const char* method;
        int returnKind;
        bool isStatic;
    };

    inline constexpr Candidate DeltaTime{"Time", "get_deltaTime", mono_metadata::TYPE_R4, true};
}
