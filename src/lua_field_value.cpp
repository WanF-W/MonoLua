/**
 * lua_value.cpp — Lua ↔ Mono 值转换中心
 * 本模块集中实现字段、数组、方法参数和返回值的 Lua/Mono 转换，
 * Class/Method/Field/Instance Binding 不复制编组代码。跨 Mono 分配保存的
 * 对象使用短期 pinned GCHandle；不完整的 ref/out 与 Nullable 语义明确拒绝。
 */
#include "lua_binding_internal.h"
#include "mono_metadata.h"
#include "mono_resolver.h"
#include "lua_value_internal.h"
#include "mono_handle.h"
#include "mono_feature_fault.h"

using namespace mono_value;
using mono::ScopedGCHandle;

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

#include "lua_value_memory.h"
using namespace mono_value_memory;
using namespace mono_metadata;

namespace
{
    bool ReadFieldSafely(MonoResolver& resolver, MonoDomain* domain, MonoObject* object,
                         MonoClassField* field, void* value, bool isStatic, std::string& error)
    {
        MonoFeatureFault fault;
        fault.stage = isStatic ? "static field read" : "instance field read";
        fault.catchAllMemoryAccess = true;
        bool success = false;
        __try
        {
            if (isStatic) success = resolver.ReadStaticField(domain, field, value);
            else
            {
                success = resolver.ReadField(object, field, value);
            }
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (ConsumeNativeCallFault(fault.stage, error)) return false;
        if (fault.code) fault.Describe(fault.stage, error);
        else if (success) return true;
        if (error.empty()) error = isStatic ? "failed to read static field" : "failed to read instance field";
        return false;
    }

    bool WriteFieldSafely(MonoResolver& resolver, MonoDomain* domain, MonoObject* object,
                          MonoClassField* field, void* value, bool isStatic, std::string& error)
    {
        MonoFeatureFault fault;
        fault.stage = isStatic ? "static field write" : "instance field write";
        fault.catchAllMemoryAccess = true;
        bool success = false;
        __try
        {
            if (isStatic) success = resolver.WriteStaticField(domain, field, value);
            else
            {
                success = resolver.WriteField(object, field, value);
            }
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (ConsumeNativeCallFault(fault.stage, error)) return false;
        if (fault.code) fault.Describe(fault.stage, error);
        else if (success) return true;
        if (error.empty()) error = isStatic ? "failed to write static field" : "failed to write instance field";
        return false;
    }

    MonoObject* ReadFieldObjectSafely(MonoResolver& resolver, MonoDomain* domain,
                                      MonoClassField* field, MonoObject* object, std::string& error)
    {
        MonoFeatureFault fault;
        fault.stage = "value-type field read";
        fault.catchAllMemoryAccess = true;
        MonoObject* result = nullptr;
        __try
        {
            result = resolver.FieldValueObject(domain, field, object);
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (ConsumeNativeCallFault(fault.stage, error)) return nullptr;
        if (fault.code) fault.Describe(fault.stage, error);
        else if (result) return result;
        if (error.empty()) error = "failed to box value-type field";
        return nullptr;
    }

}

bool LuaBridge_ReadFieldValue(lua_State* state, MonoObject* object, MonoClassField* field, bool isStatic,
                              std::string& error)
{
    auto& resolver = MonoResolver::Instance();
    MonoType* type = resolver.FieldType(field);
    if (IsNullable(resolver, type))
    {
        error = "Nullable values are not supported";
        return false;
    }
    const int declaredKind = resolver.TypeKind(type);
    const int kind = StorageTypeKind(resolver, type);
    if (kind == TYPE_VALUETYPE)
    {
        MonoDomain* domain = isStatic ? MonoRuntime::Instance().Domain() : resolver.ObjectDomain(object);
        MonoObject* boxed = ReadFieldObjectSafely(resolver, domain, field, isStatic ? nullptr : object, error);
        if (!boxed)
        {
            if (error.empty()) error = "failed to box value-type field";
            return false;
        }
        LuaBridge_PushInstance(state, boxed);
        return true;
    }
    if (!IsReferenceType(resolver, type, declaredKind) &&
        !((kind >= TYPE_BOOLEAN && kind <= TYPE_R8) || kind == TYPE_I || kind == TYPE_U))
    {
        error = "unsupported Mono field type";
        return false;
    }
    FieldStorage value{};
    if (!ReadFieldSafely(resolver, isStatic ? MonoRuntime::Instance().Domain() : resolver.ObjectDomain(object),
                         object, field, &value, isStatic, error))
    {
        if (error.empty()) error = isStatic ? "failed to read static field" : "failed to read instance field";
        return false;
    }

    return LuaBridge_PushRawValue(state, type, &value, error);
}

bool LuaBridge_WriteFieldValue(lua_State* state, int valueIndex, MonoObject* object, MonoClassField* field,
                               bool isStatic, std::string& error)
{
    auto& resolver = MonoResolver::Instance();
    MonoType* type = resolver.FieldType(field);
    if (IsNullable(resolver, type))
    {
        error = "Nullable values are not supported";
        return false;
    }
    const int declaredKind = resolver.TypeKind(type);
    const int kind = StorageTypeKind(resolver, type);
    const bool referenceType = IsReferenceType(resolver, type, declaredKind);
    ScopedGCHandle owner(resolver, object ? resolver.CreateGCHandle(object, true) : 0);
    if (object && !owner.value)
    {
        error = "failed to pin field owner";
        return false;
    }
    if (owner.value) object = owner.Target();
    FieldStorage value{};
    if (kind == TYPE_VALUETYPE)
    {
        if (!lua_isnil(state, valueIndex) && !luaL_testudata(state, valueIndex, LuaBridgeMT::INSTANCE))
        {
            error = "value-type field requires a boxed Instance";
            return false;
        }
        MonoClass* expected = resolver.TypeClass(type);
        MonoObject* boxed = lua_isnil(state, valueIndex)
            ? resolver.NewObject(isStatic ? MonoRuntime::Instance().Domain() : resolver.ObjectDomain(object), expected)
            : LuaBridge_GetInstanceObject(state, valueIndex);
        if (!expected || !boxed || resolver.ObjectClass(boxed) != expected)
        {
            error = "boxed Instance type is incompatible with the field type";
            return false;
        }
        ScopedGCHandle boxedRoot(resolver, resolver.CreateGCHandle(boxed, true));
        if (!boxedRoot.value)
        {
            error = "failed to pin boxed value";
            return false;
        }
        void* unboxed = resolver.Unbox(boxedRoot.Target());
        if (!unboxed)
        {
            error = "failed to unbox field value";
            return false;
        }
        if (isStatic && !WriteFieldSafely(resolver, MonoRuntime::Instance().Domain(), nullptr, field, unboxed,
                                          true, error))
        {
            if (error.empty()) error = "failed to write static value-type field";
            return false;
        }
        if (!isStatic && !WriteFieldSafely(resolver, resolver.ObjectDomain(object), object, field, unboxed,
                                           false, error))
            return false;
        return true;
    }

    if ((kind >= TYPE_BOOLEAN && kind <= TYPE_R8) || kind == TYPE_I || kind == TYPE_U)
    {
        if (!LuaBridge_ReadScalarValue(state, valueIndex, kind, &value, error)) return false;
    }
    else
        switch (kind)
        {
        case TYPE_STRING:
            if (lua_isnil(state, valueIndex))
                value.object = nullptr;
            else if (lua_type(state, valueIndex) == LUA_TSTRING)
            {
                value.object = reinterpret_cast<MonoObject*>(resolver.NewString(
                    isStatic ? MonoRuntime::Instance().Domain() : resolver.ObjectDomain(object),
                    lua_tostring(state, valueIndex), lua_rawlen(state, valueIndex)));
                if (owner.value) object = owner.Target();
                if (!value.object)
                {
                    error = "failed to create Mono string in the object's domain";
                    return false;
                }
            }
            else
            {
                error = "string field value must be a string or nil";
                return false;
            }
            break;
        default:
            if (!referenceType)
            {
                error = "unsupported Mono field type";
                return false;
            }
            if (lua_isnil(state, valueIndex))
                value.object = nullptr;
            else if (luaL_testudata(state, valueIndex, LuaBridgeMT::INSTANCE))
            {
                value.object = LuaBridge_GetInstanceObject(state, valueIndex);
                MonoClass* expectedClass = resolver.TypeClass(type);
                if (expectedClass && !resolver.ObjectIsInstanceOf(value.object, expectedClass))
                {
                    error = "Instance is not compatible with the field type";
                    return false;
                }
            }
            else
            {
                error = "reference field value must be an Instance or nil";
                return false;
            }
            break;
        }

    // Mono Embedding API 在这里有一个容易混淆但非常重要的不对称：
    // ·值类型传原始值缓冲区的地址；
    // ·引用类型直接传 MonoObject*，不能再传 MonoObject**。
    // 读取引用字段仍然需要 MonoObject** 作为输出位置，不能把两条规则合并。
    ScopedGCHandle referenceRoot(
        resolver, referenceType && value.object ? resolver.CreateGCHandle(value.object, true) : 0);
    if (referenceType && value.object && !referenceRoot.value)
    {
        error = "failed to pin reference field value";
        return false;
    }
    if (referenceRoot.value) value.object = referenceRoot.Target();
    void* rawValue = referenceType ? static_cast<void*>(value.object) : static_cast<void*>(&value);
    if (!WriteFieldSafely(resolver, isStatic ? MonoRuntime::Instance().Domain() : resolver.ObjectDomain(object),
                          object, field, rawValue, isStatic, error))
    {
        if (error.empty()) error = isStatic ? "failed to write static field" : "failed to write instance field";
        return false;
    }
    return true;
}
