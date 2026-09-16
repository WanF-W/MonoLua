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
    {
        uint8_t value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read boolean value"; return false; }
        lua_pushboolean(state, value != 0);
        return true;
    }
    case TYPE_CHAR:
    case TYPE_U2:
    {
        uint16_t value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read 16-bit value"; return false; }
        lua_pushinteger(state, value);
        return true;
    }
    case TYPE_I1:
    {
        int8_t value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read signed byte value"; return false; }
        lua_pushinteger(state, value);
        return true;
    }
    case TYPE_U1:
    {
        uint8_t value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read byte value"; return false; }
        lua_pushinteger(state, value);
        return true;
    }
    case TYPE_I2:
    {
        int16_t value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read signed 16-bit value"; return false; }
        lua_pushinteger(state, value);
        return true;
    }
    case TYPE_I4:
    {
        int32_t value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read signed 32-bit value"; return false; }
        lua_pushinteger(state, value);
        return true;
    }
    case TYPE_U4:
    {
        uint32_t value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read 32-bit value"; return false; }
        lua_pushinteger(state, value);
        return true;
    }
    case TYPE_I8:
    case TYPE_I:
    {
        int64_t value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read signed 64-bit value"; return false; }
        lua_pushinteger(state, static_cast<lua_Integer>(value));
        return true;
    }
    case TYPE_U8:
    case TYPE_U:
    {
        uint64_t value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read 64-bit value"; return false; }
        lua_pushinteger(state, static_cast<lua_Integer>(value));
        return true;
    }
    case TYPE_R4:
    {
        float value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read float value"; return false; }
        lua_pushnumber(state, value);
        return true;
    }
    case TYPE_R8:
    {
        double value = 0;
        if (!ReadValueSafely(address, value)) { error = "failed to read double value"; return false; }
        lua_pushnumber(state, value);
        return true;
    }
    case TYPE_STRING: {
        MonoString* string = nullptr;
        if (!ReadValueSafely(address, string)) { error = "failed to read string value"; return false; }
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
            if (bridge_lifecycle::g_nativeCallFaulted)
            {
                error = "failed to read Mono string";
                return false;
            }
            lua_pushlstring(state, text.data(), text.size());
        }
        return true;
    }
    default:
        if (IsReferenceType(resolver, type, declaredKind))
        {
            MonoObject* object = nullptr;
            if (!ReadValueSafely(address, object))
            {
                error = "failed to read object reference";
                return false;
            }
            return LuaBridge_TryPushInstance(state, object, error);
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
