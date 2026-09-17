#include "mono_hook_metadata.h"
#include "mono_runtime.h"
#include "mono_handle.h"
#include "mono_feature_fault.h"
#include "mono_metadata.h"

namespace mono_hook_detail
{
using namespace mono_metadata;
enum class GenericMethodKind { NonGeneric, Generic, Unknown };
static GenericMethodKind InspectMethodGenericsImpl(
    const MonoRuntime& runtime, MonoMethod* method, MonoFeatureFault& fault)
{
    using Kind = GenericMethodKind;
    bridge_lifecycle::NativeStageScope stage("generic reflection inspection");
    if (!method) return Kind::Unknown;
    auto& resolver = MonoResolver::Instance();
    // 通过反射同时查询方法和声明类型，不读取 MonoClass 的私有类型表示。
    // 查询失败只能返回 Unknown，不能据此认定方法是泛型。
    fault.stage = "generic inspection: current domain";
    MonoDomain* domain = runtime.Domain();
    fault.stage = "generic inspection: mono_method_get_object";
    MonoObject* reflection = resolver.MethodObject(domain, method);
    if (!reflection) return Kind::Unknown;
    fault.stage = "generic inspection: root MethodInfo";
    mono::ScopedGCHandle root(resolver, resolver.CreateGCHandle(reflection, true));
    if (!root.value) return Kind::Unknown;
    const auto property = [&resolver, &fault](MonoObject* object, const char* getterName,
                                              const char* invokeStage) -> MonoObject* {
        if (!object) return nullptr;
        fault.stage = getterName;
        for (MonoClass* klass = resolver.ObjectClass(object); klass; klass = resolver.ClassParent(klass))
            for (MonoMethod* getter : resolver.EnumerateMethods(klass))
            {
                const char* name = resolver.MethodName(getter);
                if (!name || strcmp(name, getterName) != 0 || !resolver.MethodParameters(getter).empty()) continue;
                MonoObject* exception = nullptr;
                fault.stage = invokeStage;
                MonoObject* result = resolver.Invoke(getter, object, nullptr, &exception);
                return exception ? nullptr : result;
            }
        return nullptr;
    };
    MonoObject* genericMethod = property(root.Target(), "get_IsGenericMethod",
                                        "generic inspection: invoke IsGenericMethod");
    if (!genericMethod) return Kind::Unknown;
    fault.stage = "generic inspection: root IsGenericMethod result";
    mono::ScopedGCHandle methodResult(resolver, resolver.CreateGCHandle(genericMethod, true));
    if (!methodResult.value) return Kind::Unknown;
    fault.stage = "generic inspection: unbox IsGenericMethod";
    const auto* methodFlag = static_cast<const uint8_t*>(resolver.Unbox(methodResult.Target()));
    if (!methodFlag) return Kind::Unknown;
    if (*methodFlag)
        return Kind::Generic;
    MonoObject* declaringType = property(root.Target(), "get_DeclaringType",
                                        "generic inspection: invoke DeclaringType");
    if (!declaringType) return Kind::Unknown;
    fault.stage = "generic inspection: root declaring type";
    mono::ScopedGCHandle typeRoot(resolver, resolver.CreateGCHandle(declaringType, true));
    if (!typeRoot.value) return Kind::Unknown;
    MonoObject* genericType = property(typeRoot.Target(), "get_IsGenericType",
                                      "generic inspection: invoke IsGenericType");
    if (!genericType) return Kind::Unknown;
    fault.stage = "generic inspection: root IsGenericType result";
    mono::ScopedGCHandle typeResult(resolver, resolver.CreateGCHandle(genericType, true));
    if (!typeResult.value) return Kind::Unknown;
    fault.stage = "generic inspection: unbox IsGenericType";
    const auto* typeFlag = static_cast<const uint8_t*>(resolver.Unbox(typeResult.Target()));
    if (!typeFlag) return Kind::Unknown;
    return *typeFlag ? Kind::Generic : Kind::NonGeneric;
}

static GenericMethodKind InspectGenericsSafely(
    const MonoRuntime& runtime, MonoMethod* method, std::string& error)
{
    bridge_lifecycle::ClearNativeCallFault();
    MonoFeatureFault fault;
    using Kind = GenericMethodKind;
    Kind kind = Kind::Unknown;
    error.clear();
    __try
    {
        kind = InspectMethodGenericsImpl(runtime, method, fault);
    }
    __except (fault.Filter(GetExceptionInformation()))
    {
    }
    if (ConsumeNativeCallFault(fault.stage, error))
    {
        kind = Kind::Unknown;
        return kind;
    }
    if (fault.code) fault.Describe(fault.stage, error);
    else if (kind == Kind::Unknown)
    {
        error = fault.stage;
        error += ": no usable reflection result";
    }
    return kind;
}

    bool IsSupported(MonoType* type, bool allowVoid)
    {
        auto& resolver = MonoResolver::Instance();
        if (!type) return false;
        const int kind = resolver.TypeKind(type);
        if (allowVoid && kind == TYPE_VOID) return true;
        if (resolver.TypeIsByRef(type) || kind == TYPE_BYREF) return false;
        if (kind == TYPE_VALUETYPE)
        {
            MonoClass* klass = resolver.TypeClass(type);
            return klass && resolver.ClassIsEnum(klass);
        }
        if (kind == TYPE_GENERICINST)
        {
            // 泛型共享入口可能携带隐藏的 rgctx 参数，不能按普通 Windows
            // x64 参数槽解释。普通 runtime_invoke 不受此 Hook 限制。
            return false;
        }
        return (kind >= TYPE_BOOLEAN && kind <= TYPE_STRING) || kind == TYPE_CLASS || kind == TYPE_ARRAY ||
               kind == TYPE_I || kind == TYPE_U || kind == TYPE_OBJECT || kind == TYPE_SZARRAY;
    }

    bool InspectHookMetadata(MonoMethod* method, HookEntry& entry, std::string& error, bool& internalCall,
                             const MonoScheduler::Candidate* candidate, MonoFeatureFault& fault)
    {
        auto& resolver = MonoResolver::Instance();
        fault.stage = "hook metadata: method declaring class";
        MonoClass* klass = resolver.MethodClass(method);
        fault.stage = "hook metadata: declaring class value type";
        if (!klass || resolver.ClassIsValueType(klass))
        {
            error = "hooking methods declared by value types is not supported";
            return false;
        }
        entry.method = method;
        fault.stage = "hook metadata: parameter signature";
        entry.parameters = resolver.MethodParameters(method);
        fault.stage = "hook metadata: return signature";
        entry.returnType = resolver.MethodReturnType(method);
        fault.stage = "hook metadata: method flags";
        entry.isStatic = (resolver.MethodFlags(method) & METHOD_ATTRIBUTE_STATIC) != 0;
        if (candidate)
        {
            // 默认调度器描述可以直接确定入口不是泛型方法；
            // 不检查程序集文件名，也不要求无关的实现标志。
            if (candidate != &MonoScheduler::DeltaTime)
            {
                error = "unknown built-in scheduler candidate";
                return false;
            }
            fault.stage = "hook metadata: scheduler method identity";
            const char* className = resolver.ClassName(klass);
            const char* methodName = resolver.MethodName(method);
            // FindClass 已经按 UnityEngine 命名空间解析了声明类，
            // 不再读取一份在不同 Unity Mono 分支中可能不稳定的命名空间数据。
            if (!className || !methodName ||
                strcmp(className, candidate->klass) != 0 || strcmp(methodName, candidate->method) != 0)
            {
                error = "resolved method does not match the scheduler candidate";
                return false;
            }
            fault.stage = "hook metadata: scheduler calling convention";
            if (entry.isStatic != candidate->isStatic)
            {
                error = candidate->isStatic ? "scheduler candidate must be static" : "scheduler candidate must be an instance method";
                return false;
            }
            if (!entry.parameters.empty())
            {
                error = "scheduler candidate has " + std::to_string(entry.parameters.size()) + " parameters; expected 0";
                return false;
            }
            if (!entry.returnType || resolver.TypeIsByRef(entry.returnType))
            {
                error = "scheduler return signature is missing or by-reference";
                return false;
            }
            const int actualKind = resolver.TypeKind(entry.returnType);
            if (actualKind != candidate->returnKind)
            {
                error = "scheduler return kind " + std::to_string(actualKind) +
                        "; expected " + std::to_string(candidate->returnKind);
                return false;
            }
        }
        else
        {
            const auto kind = InspectGenericsSafely(MonoRuntime::Instance(), method, error);
            if (kind == GenericMethodKind::Unknown) return false;
            if (kind == GenericMethodKind::Generic)
            {
                error = "hooking generic methods or methods on generic types is not supported";
                return false;
            }
        }
        fault.stage = "hook metadata: parameter ABI";
        if (entry.parameters.size() > 64)
        {
            error = "hook supports at most 64 parameters";
            return false;
        }
        for (MonoType* type : entry.parameters)
            if (!IsSupported(type, false))
            {
                error = "method contains an unsupported parameter type";
                return false;
            }
        fault.stage = "hook metadata: return ABI";
        if (!IsSupported(entry.returnType, true))
        {
            error = "method has an unsupported return type";
            return false;
        }
        fault.stage = "hook metadata: implementation flags";
        internalCall =
            (resolver.MethodImplementationFlags(method) & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) != 0;
        if (internalCall)
        {
            fault.stage = "hook metadata: InternalCall ABI";
            // 默认 tick 的固定描述已经确定完整的 Windows x64 形状，
            // 也包含实例 getter 的隐含 this 槽位。固定路径不再读取命名空间或
            // 反射信息；任意用户 InternalCall 仍限制为 UnityEngine.Time 的
            // 静态标量 ABI。
            const int kind = resolver.TypeKind(entry.returnType);
            bool supportedInternalCall = false;
            if (candidate)
                supportedInternalCall = entry.isStatic == candidate->isStatic &&
                    entry.parameters.empty() && kind == candidate->returnKind;
            else
            {
                const char* ns = resolver.ClassNamespace(klass);
                const char* name = resolver.ClassName(klass);
                const char* methodName = resolver.MethodName(method);
                supportedInternalCall = entry.isStatic && entry.parameters.empty() && ns && name && methodName &&
                    strcmp(ns, "UnityEngine") == 0 && strcmp(name, "Time") == 0 &&
                    ((kind >= TYPE_BOOLEAN && kind <= TYPE_R8) || kind == TYPE_I || kind == TYPE_U);
            }
            if (!supportedInternalCall)
            {
                error = "failed to prepare native hook: unsupported InternalCall ABI";
                return false;
            }
        }
        return true;
    }

    bool InspectMetadataSafely(MonoMethod* method, HookEntry& entry, std::string& error, bool& internalCall,
                               const MonoScheduler::Candidate* candidate)
    {
        bridge_lifecycle::ClearNativeCallFault();
        MonoFeatureFault fault;
        bool success = false;
        __try
        {
            success = InspectHookMetadata(method, entry, error, internalCall, candidate, fault);
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (ConsumeNativeCallFault(fault.stage, error)) return false;
        if (success && !fault.code) return true;
        if (fault.code)
            fault.Describe(fault.stage, error);
        else if (error.empty())
            error = "hook metadata is unavailable";
        return false;
    }

    bool PrepareHook(MonoMethod* method, HookEntry& entry, std::string& error,
                     const MonoScheduler::Candidate* candidate)
    {
        bool internalCall = false;
        if (!InspectMetadataSafely(method, entry, error, internalCall, candidate)) return false;
        auto& resolver = MonoResolver::Instance();
        entry.target = internalCall ? resolver.LookupInternalCall(method) : resolver.CompileMethod(method);
        if (!entry.target)
        {
            error = internalCall
                        ? "failed to prepare native hook: internal call target is unavailable (mono_lookup_internal_call)"
                        : "failed to prepare native hook: failed to compile method";
            return false;
        }
        return true;
    }

}
