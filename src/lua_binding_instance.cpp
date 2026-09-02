/**
 * ============================================================
 * lua_binding_instance.cpp — Instance userdata 公开 API
 * ============================================================
 * Instance 只保存强 GC handle，不长期保存裸 MonoObject*。每次操作重新取得
 * 当前对象地址，因此可以承受 Mono GC 移动；__gc 负责释放 handle。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include "mono_resolver.h"
#include <sstream>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    constexpr uint32_t FIELD_ATTRIBUTE_STATIC = 0x0010;
    constexpr uint32_t FIELD_ATTRIBUTE_INIT_ONLY = 0x0020;
    constexpr uint32_t FIELD_ATTRIBUTE_LITERAL = 0x0040;

    MonoClassField* FindInstanceField(MonoClass* klass, const char* name)
    {
        auto& resolver = MonoResolver::Instance();
        for (MonoClass* current = klass; current; current = resolver.ClassParent(current))
        {
            MonoClassField* field = resolver.FindField(current, name);
            if (field) return field;
        }
        return nullptr;
    }

    int Instance_Call(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        const char* name = luaL_checkstring(state, 2);
        auto& resolver = MonoResolver::Instance();
        for (MonoClass* current = resolver.ObjectClass(object);
            current; current = resolver.ClassParent(current))
        {
            MonoMethod* matched = nullptr;
            int bestScore = INT32_MIN;
            bool ambiguous = false;
            for (MonoMethod* method : resolver.EnumerateMethods(current))
            {
                const char* methodName = resolver.MethodName(method);
                if (!methodName || strcmp(methodName, name) != 0) continue;
                if ((resolver.MethodFlags(method) & 0x0010) != 0) continue;
                const auto parameters = resolver.MethodParameters(method);
                const int score = LuaBridge_ScoreMethodArguments(state, 3, parameters);
                if (score > bestScore)
                {
                    bestScore = score;
                    matched = method;
                    ambiguous = false;
                }
                else if (score != INT32_MIN && score == bestScore)
                {
                    ambiguous = true;
                }
            }
            // 派生类声明的方法优先于父类同签名方法，避免 override 与基类
            // 方法同时进入候选后被误报成重载歧义。
            if (ambiguous)
                return luaL_error(state,
                    "ambiguous overload: %s; use Class:get_method() and Method:call()",
                    name);
            if (matched) return LuaBridge_InvokeMethod(state, matched, object, 3);
        }
        return luaL_error(state, "method not found or arguments are incompatible: %s", name);
    }

    int Instance_ReadField(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        const char* name = luaL_checkstring(state, 2);
        auto& resolver = MonoResolver::Instance();
        MonoClassField* field = FindInstanceField(resolver.ObjectClass(object), name);
        if (!field) return luaL_error(state, "field not found: %s", name);
        if ((resolver.FieldFlags(field) & FIELD_ATTRIBUTE_STATIC) != 0)
            return luaL_error(state, "field is static; use Field:read() or Class:read_static_field(): %s", name);

        std::string error;
        if (!LuaBridge_ReadFieldValue(state, object, field, false, error))
            return luaL_error(state, "%s: %s", name, error.c_str());
        return 1;
    }

    int Instance_WriteField(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        const char* name = luaL_checkstring(state, 2);
        auto& resolver = MonoResolver::Instance();
        MonoClassField* field = FindInstanceField(resolver.ObjectClass(object), name);
        if (!field) return luaL_error(state, "field not found: %s", name);
        const uint32_t flags = resolver.FieldFlags(field);
        if ((flags & FIELD_ATTRIBUTE_STATIC) != 0)
            return luaL_error(state, "field is static; use Field:write() or Class:write_static_field(): %s", name);
        if ((flags & (FIELD_ATTRIBUTE_INIT_ONLY | FIELD_ATTRIBUTE_LITERAL)) != 0)
            return luaL_error(state, "field is readonly or const: %s", name);

        std::string error;
        if (!LuaBridge_WriteFieldValue(state, 3, object, field, false, error))
            return luaL_error(state, "%s: %s", name, error.c_str());
        return 0;
    }

    int Instance_GetClass(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        LuaBridge_PushClass(state, MonoResolver::Instance().ObjectClass(object));
        return 1;
    }

    int Instance_GetAddress(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(object)));
        return 1;
    }

    int Instance_ToString(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        MonoClass* klass = MonoResolver::Instance().ObjectClass(object);
        const char* name = MonoResolver::Instance().ClassName(klass);
        char text[256]{};
        sprintf_s(text, "Instance: %s @ 0x%llX",
            name ? name : "<invalid>",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(object)));
        lua_pushstring(state, text);
        return 1;
    }

    int Instance_GC(lua_State* state)
    {
        auto* userdata = static_cast<LuaInstanceUD*>(
            luaL_testudata(state, 1, LuaBridgeMT::INSTANCE));
        if (userdata && userdata->gcHandle)
        {
            MonoResolver::Instance().FreeGCHandle(userdata->gcHandle);
            userdata->gcHandle = 0;
        }
        return 0;
    }

    int Instance_Index(lua_State* state)
    {
        if (lua_type(state, 2) == LUA_TNUMBER)
            return LuaBridge_ContainerIndex(
                state, LuaBridge_GetInstanceObject(state, 1), luaL_checkinteger(state, 2));
        luaL_checktype(state, 2, LUA_TSTRING);
        lua_getmetatable(state, 1);
        lua_pushvalue(state, 2);
        lua_rawget(state, -2);
        return 1;
    }

    int Instance_NewIndex(lua_State* state)
    {
        if (lua_type(state, 2) != LUA_TNUMBER)
            return luaL_error(state, "cannot set arbitrary fields on Instance; use write_field()");
        return LuaBridge_ContainerNewIndex(
            state, LuaBridge_GetInstanceObject(state, 1), luaL_checkinteger(state, 2), 3);
    }

    int Instance_Len(lua_State* state)
    {
        lua_pushinteger(state, LuaBridge_ContainerLength(
            state, LuaBridge_GetInstanceObject(state, 1)));
        return 1;
    }

    int Instance_Each(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        const int length = LuaBridge_ContainerLength(state, object);
        for (int index = 1; index <= length; ++index)
        {
            lua_pushvalue(state, 2);
            LuaBridge_ContainerIndex(state, object, index);
            lua_pushinteger(state, index);
            lua_call(state, 2, 0);
        }
        return 0;
    }

    int Instance_Dump(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        const bool includeParents = lua_toboolean(state, 2) != 0;
        auto& resolver = MonoResolver::Instance();
        std::ostringstream output;

        if (LuaBridge_IsArray(object) || LuaBridge_IsList(object))
        {
            const int length = LuaBridge_ContainerLength(state, object);
            const char* containerName = resolver.ClassName(resolver.ObjectClass(object));
            output << "Instance: " << (containerName ? containerName : "?")
                << " (" << length << " elements):\n";
            for (int index = 1; index <= length; ++index)
            {
                LuaBridge_ContainerIndex(state, object, index);
                size_t size = 0;
                const char* value = luaL_tolstring(state, -1, &size);
                output << "  [" << index << "] = " << (value ? std::string(value, size) : "?") << '\n';
                lua_pop(state, 2);
            }
        }
        else
        {
            for (MonoClass* current = resolver.ObjectClass(object); current;
                current = includeParents ? resolver.ClassParent(current) : nullptr)
            {
                const char* name = resolver.ClassName(current);
                output << (name ? name : "?") << ":\n";
                for (MonoClassField* field : resolver.EnumerateFields(current))
                {
                    if ((resolver.FieldFlags(field) & FIELD_ATTRIBUTE_STATIC) != 0) continue;
                    output << "  " << (resolver.FieldName(field) ? resolver.FieldName(field) : "?") << " = ";
                    std::string error;
                    if (LuaBridge_ReadFieldValue(state, object, field, false, error))
                    {
                        size_t size = 0;
                        const char* value = luaL_tolstring(state, -1, &size);
                        output << (value ? std::string(value, size) : "?");
                        lua_pop(state, 2);
                    }
                    else output << "<" << error << ">";
                    output << '\n';
                }
                if (!includeParents) break;
                output << '\n';
            }
        }
        std::string text = output.str();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        lua_pushlstring(state, text.data(), text.size());
        return 1;
    }
}

const luaL_Reg* LuaBinding_GetInstanceMethods()
{
    static const luaL_Reg methods[] = {
        {"call", Instance_Call},
        {"read_field", Instance_ReadField},
        {"write_field", Instance_WriteField},
        {"get_class", Instance_GetClass},
        {"get_address", Instance_GetAddress},
        {"each", Instance_Each},
        {"dump", Instance_Dump},
        {"__tostring", Instance_ToString},
        {"__index", Instance_Index},
        {"__newindex", Instance_NewIndex},
        {"__len", Instance_Len},
        {"__gc", Instance_GC},
        {nullptr, nullptr}
    };
    return methods;
}
