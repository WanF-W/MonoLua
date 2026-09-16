/**
 * unity_object_query.cpp — Unity 专属对象查询实现
 * 查询结果先通过临时 GCHandle 保持数组稳定，再为每个对象创建强
 * GCHandle 交给 Lua Instance userdata。这样即使遍历中的 Mono 调用触发
 * 移动式 GC，也不会继续使用已经失效的裸地址。
 */
#include "unity_object_query.h"

#include "mono_metadata.h"
#include "mono_feature_fault.h"
#include "mono_resolver.h"
#include "mono_runtime.h"

namespace UnityObjectQuery
{
    static bool FindObjectsOfTypeImpl(MonoClass* klass, mono::GCHandles& handles,
                                      std::string& error, MonoFeatureFault& fault)
    {
        error.clear();
        bridge_lifecycle::NativeStageScope stage("Unity object query");
        if (!klass)
        {
            error = "class is null";
            return false;
        }

        auto& resolver = MonoResolver::Instance();
        fault.stage = "Unity object query: array exports";
        if (!resolver.CanReadArrays())
        {
            error = "Mono array reading exports are unavailable";
            return false;
        }
        fault.stage = "Unity object query: UnityEngine.Object class";
        MonoClass* unityObject = MonoRuntime::Instance().FindClass("UnityEngine", "Object");
        if (!unityObject)
        {
            error = "UnityEngine.Object was not found in the loaded assemblies";
            return false;
        }

        bool derivesFromUnityObject = false;
        for (MonoClass* current = klass; current; current = resolver.ClassParent(current))
        {
            if (current == unityObject)
            {
                derivesFromUnityObject = true;
                break;
            }
        }
        if (!derivesFromUnityObject)
        {
            error = "class does not derive from UnityEngine.Object";
            return false;
        }

        // TypeObject 是 Mono 管理的对象。暂时保留一个强引用，直到反射调用
        // 完成，避免中间的元数据查询或运行时调用触发 GC 后指针失效。
        fault.stage = "Unity object query: System.Type";
        MonoObject* typeObject =
            resolver.TypeObject(MonoRuntime::Instance().Domain(), resolver.ClassType(klass));
        if (!typeObject)
        {
            error = "failed to obtain System.Type for class";
            return false;
        }
        mono::ScopedGCHandle typeHandle(resolver, resolver.CreateGCHandle(typeObject, true));
        if (!typeHandle.value)
        {
            error = "failed to retain System.Type for class";
            return false;
        }
        typeObject = typeHandle.Target();
        if (!typeObject)
        {
            error = "System.Type is no longer available";
            return false;
        }

        MonoMethod* findMethod = nullptr;
        bool usesSortMode = false;
        fault.stage = "Unity object query: FindObjectsOfType overload";
        for (MonoMethod* method : resolver.EnumerateMethods(unityObject))
        {
            const char* name = resolver.MethodName(method);
            if (!name || strcmp(name, "FindObjectsOfType") != 0) continue;
            if ((resolver.MethodFlags(method) & mono_metadata::METHOD_ATTRIBUTE_STATIC) == 0) continue;
            const auto parameterNames = resolver.MethodParameterTypes(method);
            if (parameterNames.size() == 1 && parameterNames[0] == "System.Type")
            {
                findMethod = method;
                break;
            }
            if (!findMethod && parameterNames.size() == 2 && parameterNames[0] == "System.Type" &&
                parameterNames[1] == "System.Boolean")
            {
                findMethod = method;
            }
        }

        // Unity 2023+ 逐步以 FindObjectsByType(Type, FindObjectsSortMode)
        // 取代旧入口。None 的枚举值为 0，不依赖 Unity 私有布局。
        if (!findMethod)
        {
            fault.stage = "Unity object query: FindObjectsByType overload";
            for (MonoMethod* method : resolver.EnumerateMethods(unityObject))
            {
                const char* name = resolver.MethodName(method);
                if (!name || strcmp(name, "FindObjectsByType") != 0) continue;
                if ((resolver.MethodFlags(method) & mono_metadata::METHOD_ATTRIBUTE_STATIC) == 0) continue;
                const auto parameterNames = resolver.MethodParameterTypes(method);
                if (parameterNames.size() == 2 && parameterNames[0] == "System.Type" &&
                    parameterNames[1].find("FindObjectsSortMode") != std::string::npos)
                {
                    findMethod = method;
                    usesSortMode = true;
                    break;
                }
            }
        }
        if (!findMethod)
        {
            error = "UnityEngine.Object.FindObjectsOfType/FindObjectsByType was not found";
            return false;
        }

        uint8_t includeInactive = 0;
        int32_t sortModeNone = 0;
        void* parameters[2] = {typeObject, usesSortMode ? static_cast<void*>(&sortModeNone)
                                                        : static_cast<void*>(&includeInactive)};
        MonoObject* exception = nullptr;
        fault.stage = usesSortMode ? "Unity object query: invoke FindObjectsByType"
                                   : "Unity object query: invoke FindObjectsOfType";
        MonoObject* result = resolver.Invoke(findMethod, nullptr, parameters, &exception);
        if (exception)
        {
            error = resolver.ObjectString(exception);
            if (error.empty()) error = "FindObjectsOfType threw a managed exception";
            return false;
        }
        if (!result) return true;

        fault.stage = "Unity object query: read result array";
        mono::ScopedGCHandle arrayHandle(resolver, resolver.CreateGCHandle(result, true));
        if (!arrayHandle.value)
        {
            error = "failed to retain FindObjectsOfType result array";
            return false;
        }
        MonoArray* array = reinterpret_cast<MonoArray*>(arrayHandle.Target());
        const uintptr_t length = resolver.ArrayLength(array);
        handles.values.reserve(static_cast<size_t>(length));
        for (uintptr_t index = 0; index < length; ++index)
        {
            array = reinterpret_cast<MonoArray*>(arrayHandle.Target());
            MonoObject* object = resolver.ArrayElement(array, index);
            if (!object) continue;
            const MonoGCHandle objectHandle = resolver.CreateGCHandle(object);
            if (!objectHandle)
            {
                error = "failed to retain a Unity object result";
                return false;
            }
            handles.values.push_back(objectHandle);
        }
        return true;
    }

    bool FindObjectsOfType(MonoClass* klass, mono::GCHandles& handles, std::string& error)
    {
        bridge_lifecycle::ClearNativeCallFault();
        MonoFeatureFault fault;
        bool success = false;
        fault.catchAllMemoryAccess = true;
        __try
        {
            success = FindObjectsOfTypeImpl(klass, handles, error, fault);
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (ConsumeNativeCallFault(fault.stage, error)) return false;
        if (success) return true;
        if (fault.code)
            fault.Describe(fault.stage, error);
        else if (error.empty())
            error = "Unity object query failed";
        return false;
    }
} // namespace UnityObjectQuery
