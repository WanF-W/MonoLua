/**
 * unity_object_query.h — Unity 专属查询能力
 * 这里承载 UnityEngine.Object 等业务语义。MonoRuntime 只提供通用
 * 运行时能力，不因为 MonoLua 当前主要服务 Unity 就被迫知道 Unity API。
 */
#pragma once

#include "mono_handle.h"

namespace UnityObjectQuery
{
    bool FindObjectsOfType(MonoClass* klass, mono::GCHandles& handles, std::string& error);
}
