/**
 * ============================================================
 * lua_binding_field.cpp — Field userdata 公开 API
 * ============================================================
 * 同时提供实例/静态字段读写；具体类型转换全部进入 lua_value，Binding
 * 只校验调用形式、字段修饰符和 Instance 兼容性。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include "mono_resolver.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    constexpr uint32_t FIELD_ATTRIBUTE_STATIC = 0x0010;
    constexpr uint32_t FIELD_ATTRIBUTE_INIT_ONLY = 0x0020;
    constexpr uint32_t FIELD_ATTRIBUTE_LITERAL = 0x0040;

    int Field_GetName(lua_State* state)
    {
        const LuaFieldUD* userdata = LuaBridge_CheckField(state, 1);
        const char* name = MonoResolver::Instance().FieldName(userdata->field);
        lua_pushstring(state, name ? name : "");
        return 1;
    }

    int Field_GetClass(lua_State* state)
    {
        const LuaFieldUD* userdata = LuaBridge_CheckField(state, 1);
        LuaBridge_PushClass(state, MonoResolver::Instance().FieldClass(userdata->field));
        return 1;
    }

    int Field_GetSignature(lua_State* state)
    {
        const LuaFieldUD* userdata = LuaBridge_CheckField(state, 1);
        const std::string signature = MonoResolver::Instance().FieldSignature(userdata->field);
        lua_pushlstring(state, signature.data(), signature.size());
        return 1;
    }

    int Field_GetOffset(lua_State* state)
    {
        const LuaFieldUD* userdata = LuaBridge_CheckField(state, 1);
        const int32_t offset = MonoResolver::Instance().FieldOffset(userdata->field);
        if (offset < 0)
        {
            lua_pushnil(state);
            return 1;
        }
        lua_pushinteger(state, static_cast<lua_Integer>(offset));
        return 1;
    }

    int Field_Read(lua_State* state)
    {
        const LuaFieldUD* userdata = LuaBridge_CheckField(state, 1);
        auto& resolver = MonoResolver::Instance();
        const bool isStatic = (resolver.FieldFlags(userdata->field) & FIELD_ATTRIBUTE_STATIC) != 0;

        MonoObject* object = isStatic ? nullptr : LuaBridge_GetInstanceObject(state, 2);
        if (!isStatic && !resolver.ObjectIsInstanceOf(object, resolver.FieldClass(userdata->field)))
            return luaL_error(state, "instance is not compatible with the field's declaring class");
        std::string error;
        if (!LuaBridge_ReadFieldValue(state, object, userdata->field, isStatic, error))
            return luaL_error(state, "%s", error.c_str());
        return 1;
    }

    int Field_Write(lua_State* state)
    {
        const LuaFieldUD* userdata = LuaBridge_CheckField(state, 1);
        auto& resolver = MonoResolver::Instance();
        const uint32_t flags = resolver.FieldFlags(userdata->field);
        const bool isStatic = (flags & FIELD_ATTRIBUTE_STATIC) != 0;
        if ((flags & (FIELD_ATTRIBUTE_INIT_ONLY | FIELD_ATTRIBUTE_LITERAL)) != 0)
            return luaL_error(state, "field is readonly or const");

        MonoObject* object = isStatic ? nullptr : LuaBridge_GetInstanceObject(state, 2);
        if (!isStatic && !resolver.ObjectIsInstanceOf(object, resolver.FieldClass(userdata->field)))
            return luaL_error(state, "instance is not compatible with the field's declaring class");
        std::string error;
        const int valueIndex = isStatic ? 2 : 3;
        if (!LuaBridge_WriteFieldValue(
            state, valueIndex, object, userdata->field, isStatic, error))
            return luaL_error(state, "%s", error.c_str());
        return 0;
    }
}

const luaL_Reg* LuaBinding_GetFieldMethods()
{
    static const luaL_Reg methods[] = {
        {"get_name", Field_GetName},
        {"get_class", Field_GetClass},
        {"get_signature", Field_GetSignature},
        {"get_offset", Field_GetOffset},
        {"read", Field_Read},
        {"write", Field_Write},
        {"__tostring", Field_GetSignature},
        {nullptr, nullptr}
    };
    return methods;
}
