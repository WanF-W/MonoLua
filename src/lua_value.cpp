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

using namespace mono_value;
using mono::ScopedGCHandle;

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    using namespace mono_metadata;

    union FieldStorage {
        int8_t i1;
        uint8_t u1;
        int16_t i2;
        uint16_t u2;
        int32_t i4;
        uint32_t u4;
        int64_t i8;
        uint64_t u8;
        float r4;
        double r8;
        intptr_t nativeInt;
        uintptr_t nativeUInt;
        MonoObject* object;
    };

} // namespace

bool LuaBridge_ReadScalarValue(lua_State* state, int index, int kind, void* storage, std::string& error)
{
    FieldStorage value{};
    if ((kind == TYPE_I || kind == TYPE_U) && lua_islightuserdata(state, index))
        value.nativeUInt = reinterpret_cast<uintptr_t>(lua_touserdata(state, index));
    else if ((kind >= TYPE_CHAR && kind <= TYPE_U8) || kind == TYPE_I || kind == TYPE_U)
    {
        int exact = 0;
        const lua_Integer integer = lua_tointegerx(state, index, &exact);
        if (lua_type(state, index) != LUA_TNUMBER || !exact)
        {
            error = "value must be an integer";
            return false;
        }
        if (!IntegerFits(kind, integer))
        {
            error = "integer is outside the target type range";
            return false;
        }
        switch (kind)
        {
        case TYPE_CHAR:
        case TYPE_U2:
            value.u2 = static_cast<uint16_t>(integer);
            break;
        case TYPE_I1:
            value.i1 = static_cast<int8_t>(integer);
            break;
        case TYPE_U1:
            value.u1 = static_cast<uint8_t>(integer);
            break;
        case TYPE_I2:
            value.i2 = static_cast<int16_t>(integer);
            break;
        case TYPE_I4:
            value.i4 = static_cast<int32_t>(integer);
            break;
        case TYPE_U4:
            value.u4 = static_cast<uint32_t>(integer);
            break;
        case TYPE_I8:
        case TYPE_I:
            value.i8 = integer;
            break;
        case TYPE_U8:
        case TYPE_U:
            value.u8 = static_cast<uint64_t>(integer);
            break;
        }
    }
    else if (kind == TYPE_BOOLEAN)
    {
        if (!lua_isboolean(state, index))
        {
            error = "value must be a boolean";
            return false;
        }
        value.u1 = static_cast<uint8_t>(lua_toboolean(state, index));
    }
    else if (kind == TYPE_R4 || kind == TYPE_R8)
    {
        if (lua_type(state, index) != LUA_TNUMBER)
        {
            error = "value must be a number";
            return false;
        }
        if (kind == TYPE_R4)
            value.r4 = static_cast<float>(lua_tonumber(state, index));
        else
            value.r8 = lua_tonumber(state, index);
    }
    else
    {
        error = "unsupported scalar type";
        return false;
    }
    static_assert(sizeof(value) == sizeof(uint64_t));
    memcpy(storage, &value, sizeof(value));
    return true;
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
        MonoObject* boxed = resolver.FieldValueObject(domain, field, isStatic ? nullptr : object);
        if (!boxed)
        {
            error = "failed to box value-type field";
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
    if (isStatic)
    {
        if (!resolver.ReadStaticField(MonoRuntime::Instance().Domain(), field, &value))
        {
            error = "failed to read static field";
            return false;
        }
    }
    else
        resolver.ReadField(object, field, &value);

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
        if (isStatic && !resolver.WriteStaticField(MonoRuntime::Instance().Domain(), field, unboxed))
        {
            error = "failed to write static value-type field";
            return false;
        }
        if (!isStatic) resolver.WriteField(object, field, unboxed);
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
    if (isStatic)
    {
        if (!resolver.WriteStaticField(MonoRuntime::Instance().Domain(), field, rawValue))
        {
            error = "failed to write static field";
            return false;
        }
    }
    else
        resolver.WriteField(object, field, rawValue);
    return true;
}

bool LuaBridge_PushRawValue(lua_State* state, MonoType* type, void* address, std::string& error)
{
    if (!type || !address)
    {
        error = "value address is null";
        return false;
    }
    auto& resolver = MonoResolver::Instance();
    if (IsNullable(resolver, type))
    {
        error = "Nullable values are not supported";
        return false;
    }
    const int declaredKind = resolver.TypeKind(type);
    const int kind = StorageTypeKind(resolver, type);
    switch (kind)
    {
    case TYPE_BOOLEAN:
        lua_pushboolean(state, *static_cast<uint8_t*>(address) != 0);
        return true;
    case TYPE_CHAR:
    case TYPE_U2:
        lua_pushinteger(state, *static_cast<uint16_t*>(address));
        return true;
    case TYPE_I1:
        lua_pushinteger(state, *static_cast<int8_t*>(address));
        return true;
    case TYPE_U1:
        lua_pushinteger(state, *static_cast<uint8_t*>(address));
        return true;
    case TYPE_I2:
        lua_pushinteger(state, *static_cast<int16_t*>(address));
        return true;
    case TYPE_I4:
        lua_pushinteger(state, *static_cast<int32_t*>(address));
        return true;
    case TYPE_U4:
        lua_pushinteger(state, *static_cast<uint32_t*>(address));
        return true;
    case TYPE_I8:
    case TYPE_I:
        lua_pushinteger(state, static_cast<lua_Integer>(*static_cast<int64_t*>(address)));
        return true;
    case TYPE_U8:
    case TYPE_U:
        lua_pushinteger(state, static_cast<lua_Integer>(*static_cast<uint64_t*>(address)));
        return true;
    case TYPE_R4:
        lua_pushnumber(state, *static_cast<float*>(address));
        return true;
    case TYPE_R8:
        lua_pushnumber(state, *static_cast<double*>(address));
        return true;
    case TYPE_STRING: {
        MonoString* string = *static_cast<MonoString**>(address);
        if (!string)
            lua_pushnil(state);
        else
        {
            if (!resolver.CanReadStrings())
            {
                error = "Mono string reading exports are unavailable";
                return false;
            }
            const std::string text = resolver.StringValue(string);
            lua_pushlstring(state, text.data(), text.size());
        }
        return true;
    }
    default:
        if (IsReferenceType(resolver, type, declaredKind))
        {
            return LuaBridge_TryPushInstance(state, *static_cast<MonoObject**>(address), error);
        }
        if (kind == TYPE_VALUETYPE)
        {
            MonoObject* boxed =
                resolver.Box(MonoRuntime::Instance().Domain(), resolver.TypeClass(type), address);
            if (!boxed)
            {
                error = "failed to box array element";
                return false;
            }
            return LuaBridge_TryPushInstance(state, boxed, error);
        }
        error = "unsupported container element type";
        return false;
    }
}

bool LuaBridge_WriteArrayValue(lua_State* state, int valueIndex, MonoArray* array, MonoType* type,
                               uintptr_t index, std::string& error)
{
    if (!array || !type)
    {
        error = "array or element type is null";
        return false;
    }
    auto& resolver = MonoResolver::Instance();
    // ArrayAddress 返回的是数组内部地址。即使调用方持有普通强 handle，
    // 中间的 Mono API 也可能触发移动式 GC；这里固定数组直到本次写入结束。
    ScopedGCHandle arrayHandle(resolver, resolver.CreateGCHandle(reinterpret_cast<MonoObject*>(array), true));
    if (!arrayHandle.value)
    {
        error = "failed to pin the Mono array";
        return false;
    }
    array = reinterpret_cast<MonoArray*>(arrayHandle.Target());
    if (!array)
    {
        error = "Mono array is no longer available";
        return false;
    }
    MonoClass* arrayClass = resolver.ObjectClass(reinterpret_cast<MonoObject*>(array));
    const int elementSize = resolver.ArrayElementSize(arrayClass);
    if (elementSize <= 0)
    {
        error = "array element size is invalid";
        return false;
    }
    void* address = resolver.ArrayAddress(array, elementSize, index);
    if (!address)
    {
        error = "array element address is null";
        return false;
    }
    if (IsNullable(resolver, type))
    {
        error = "Nullable values are not supported";
        return false;
    }
    const int declaredKind = resolver.TypeKind(type);
    const int kind = StorageTypeKind(resolver, type);
    if ((kind >= TYPE_BOOLEAN && kind <= TYPE_R8) || kind == TYPE_I || kind == TYPE_U)
    {
        uint64_t storage = 0;
        if (elementSize > static_cast<int>(sizeof(storage)))
        {
            error = "invalid scalar array element size";
            return false;
        }
        if (!LuaBridge_ReadScalarValue(state, valueIndex, kind, &storage, error)) return false;
        memcpy(address, &storage, static_cast<size_t>(elementSize));
        return true;
    }
    if (kind == TYPE_VALUETYPE)
    {
        if (!lua_isnil(state, valueIndex) && !luaL_testudata(state, valueIndex, LuaBridgeMT::INSTANCE))
        {
            error = "value-type array element requires a boxed Instance";
            return false;
        }
        MonoObject* boxed = nullptr;
        MonoClass* expected = resolver.TypeClass(type);
        if (lua_isnil(state, valueIndex))
            boxed = resolver.NewObject(resolver.ObjectDomain(reinterpret_cast<MonoObject*>(array)), expected);
        else if (!LuaBridge_TryGetInstanceObject(state, valueIndex, boxed))
        {
            error = "boxed value-type Instance is no longer available";
            return false;
        }
        if (!expected || !boxed || resolver.ObjectClass(boxed) != expected)
        {
            error = "boxed Instance type is incompatible with array element type";
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
            error = "failed to unbox array element";
            return false;
        }
        if (!resolver.CopyValue(address, unboxed, expected))
        {
            error = "this Mono runtime cannot copy value-type array elements";
            return false;
        }
        return true;
    }
    if (!IsReferenceType(resolver, type, declaredKind))
    {
        error = "unsupported Mono array element type";
        return false;
    }
    MonoObject* value = nullptr;
    if (!lua_isnil(state, valueIndex))
    {
        if (kind == TYPE_STRING)
        {
            if (lua_type(state, valueIndex) != LUA_TSTRING)
            {
                error = "array value must be a string or nil";
                return false;
            }
            value = reinterpret_cast<MonoObject*>(
                resolver.NewString(resolver.ObjectDomain(reinterpret_cast<MonoObject*>(array)),
                                   lua_tostring(state, valueIndex), lua_rawlen(state, valueIndex)));
            if (!value)
            {
                error = "failed to create the Mono string array value";
                return false;
            }
        }
        else
        {
            if (!LuaBridge_TryGetInstanceObject(state, valueIndex, value))
            {
                error = "array value must be a valid Instance or nil";
                return false;
            }
            MonoClass* expected = resolver.TypeClass(type);
            if (expected && !resolver.ObjectIsInstanceOf(value, expected))
            {
                error = "Instance type is incompatible with array element type";
                return false;
            }
        }
    }
    ScopedGCHandle valueHandle(resolver, value ? resolver.CreateGCHandle(value, true) : 0);
    if (value && !valueHandle.value)
    {
        error = "failed to pin the array element";
        return false;
    }
    // The array is pinned for this entire operation; all reference writes use Mono's barrier.
    if (!resolver.SetArrayReference(array, address, valueHandle.Target()))
    {
        error = "this Mono runtime cannot write reference array elements";
        return false;
    }
    return true;
}
