/**
 * lua_binding_instance.cpp — Instance userdata 公开 API
 * Instance 只保存强 GC handle，不长期保存裸 MonoObject*。每次操作重新取得
 * 当前对象地址，因此可以承受 Mono GC 移动；显式刷新发现程序集移除后会拒绝旧对象。
 * __gc 负责释放 handle。
 */
#include "lua_binding_internal.h"
#include "mono_metadata.h"
#include "mono_resolver.h"
#include "lua_dump.h"
#include "lua_engine.h"
#include <sstream>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace
{

    int Instance_Call(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        const char* name = luaL_checkstring(state, 2);
        auto& resolver = MonoResolver::Instance();
        MonoMethod* matched = LuaBridge_FindBestMethod(state, resolver.ObjectClass(object), name, 3, false);
        if (matched) return LuaBridge_InvokeMethod(state, matched, object, 3);
        return LuaEngine::RaiseBridgeError(state, "method not found or arguments are incompatible: %s", name);
    }

    int Instance_ReadField(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        const char* name = luaL_checkstring(state, 2);
        auto& resolver = MonoResolver::Instance();
        MonoClassField* field = resolver.FindFieldInHierarchy(resolver.ObjectClass(object), name);
        if (!field) return LuaEngine::RaiseBridgeError(state, "field not found: %s", name);
        if ((resolver.FieldFlags(field) & mono_metadata::FIELD_ATTRIBUTE_STATIC) != 0)
            return LuaEngine::RaiseBridgeError(state, "field is static; use Field:read() or Class:read_static_field(): %s",
                              name);

        std::string error;
        if (!LuaBridge_ReadFieldValue(state, LuaBridge_GetInstanceObject(state, 1), field, false, error))
            return LuaEngine::RaiseBridgeError(state, "%s: %s", name, error.c_str());
        return 1;
    }

    int Instance_WriteField(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        const char* name = luaL_checkstring(state, 2);
        auto& resolver = MonoResolver::Instance();
        MonoClassField* field = resolver.FindFieldInHierarchy(resolver.ObjectClass(object), name);
        if (!field) return LuaEngine::RaiseBridgeError(state, "field not found: %s", name);
        const uint32_t flags = resolver.FieldFlags(field);
        if ((flags & mono_metadata::FIELD_ATTRIBUTE_STATIC) != 0)
            return LuaEngine::RaiseBridgeError(state, "field is static; use Field:write() or Class:write_static_field(): %s",
                              name);
        if ((flags & mono_metadata::FIELD_ATTRIBUTE_LITERAL) != 0)
            return LuaEngine::RaiseBridgeError(state, "const field cannot be written: %s", name);

        std::string error;
        if (!LuaBridge_WriteFieldValue(state, 3, object, field, false, error))
            return LuaEngine::RaiseBridgeError(state, "%s: %s", name, error.c_str());
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

    std::string FullTypeName(MonoClass* klass)
    {
        auto& resolver = MonoResolver::Instance();
        return resolver.TypeDisplayName(resolver.ClassType(klass));
    }

    int Instance_LengthForDisplay(lua_State* state)
    {
        lua_pushinteger(state, LuaBridge_ContainerLength(state, LuaBridge_GetInstanceObject(state, 1)));
        return 1;
    }

    int Instance_ToString(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        MonoClass* klass = MonoResolver::Instance().ObjectClass(object);
        std::string name = FullTypeName(klass);
        if (LuaBridge_IsArray(object) || LuaBridge_IsList(object))
        {
            lua_pushcfunction(state, Instance_LengthForDisplay);
            lua_pushvalue(state, 1);
            const int status = lua_pcall(state, 1, 1, 0);
            LuaEngine::Instance().CheckHealthy();
            if (status == LUA_OK) name += " (count=" + std::to_string(lua_tointeger(state, -1)) + ')';
            lua_pop(state, 1);
        }
        lua_pushfstring(state, "Instance: %s @ %p", name.c_str(),
                        static_cast<void*>(LuaBridge_GetInstanceObject(state, 1)));
        return 1;
    }

    int Instance_GC(lua_State* state)
    {
        auto* userdata = static_cast<LuaInstanceUD*>(luaL_testudata(state, 1, LuaBridgeMT::INSTANCE));
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
        {
            if (!lua_isinteger(state, 2)) return LuaEngine::RaiseBridgeError(state, "container index must be an integer");
            return LuaBridge_ContainerIndex(state, LuaBridge_GetInstanceObject(state, 1),
                                            luaL_checkinteger(state, 2));
        }
        luaL_checktype(state, 2, LUA_TSTRING);
        lua_getmetatable(state, 1);
        lua_pushvalue(state, 2);
        lua_rawget(state, -2);
        return 1;
    }

    int Instance_NewIndex(lua_State* state)
    {
        if (!lua_isinteger(state, 2))
            return LuaEngine::RaiseBridgeError(state, "cannot set arbitrary fields on Instance; use write_field()");
        return LuaBridge_ContainerNewIndex(state, LuaBridge_GetInstanceObject(state, 1),
                                           luaL_checkinteger(state, 2), 3);
    }

    int Instance_Len(lua_State* state)
    {
        lua_pushinteger(state, LuaBridge_ContainerLength(state, LuaBridge_GetInstanceObject(state, 1)));
        return 1;
    }

    int Instance_Each(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        const int length = LuaBridge_ContainerLength(state, object);
        for (lua_Integer index = 1; index <= length; ++index)
        {
            lua_pushvalue(state, 2);
            LuaBridge_ContainerIndex(state, LuaBridge_GetInstanceObject(state, 1), index);
            lua_pushinteger(state, index);
            if (lua_pcall(state, 2, 0, 0) != LUA_OK) return lua_error(state);
        }
        return 0;
    }

    int Instance_Dump(lua_State* state)
    {
        MonoObject* object = LuaBridge_GetInstanceObject(state, 1);
        const bool includeParents = lua_toboolean(state, 2) != 0;
        auto& resolver = MonoResolver::Instance();
        LuaDumpStream output;
        MonoClass* actualClass = resolver.ObjectClass(object);

        if (LuaBridge_IsArray(object) || LuaBridge_IsList(object))
        {
            const bool isArray = LuaBridge_IsArray(object);
            const int length = LuaBridge_ContainerLength(state, object);
            const std::string containerName = FullTypeName(actualClass);
            output << (isArray ? "Array: " : "List: ")
                   << containerName << "\ncount: " << length << '\n';
            for (lua_Integer index = 1; index <= length; ++index)
            {
                if (output.Full()) { output << " "; break; }
                LuaBridge_ContainerIndex(state, LuaBridge_GetInstanceObject(state, 1), index);
                std::string value;
                if (LuaBridge_TryToString(state, -1, value))
                {
                    const bool quote = lua_type(state, -1) == LUA_TSTRING;
                    output << '[' << index << "] = " << (quote ? "\"" : "") << value << (quote ? "\"" : "")
                           << '\n';
                }
                else
                    output << "  [" << index << "] = <tostring error: " << value << ">\n";
                lua_pop(state, 1);
            }
        }
        else
        {
            const char* nameSpace = resolver.ClassNamespace(actualClass);
            output << "Instance: " << (nameSpace && *nameSpace ? std::string(nameSpace) + '.' : "")
                   << resolver.ClassName(actualClass) << '\n';
            const char* assembly = resolver.ImageName(resolver.ClassImage(actualClass));
            output << "Assembly: " << (assembly ? assembly : "?") << '\n';
            output << "Address: 0x" << std::hex << std::uppercase << reinterpret_cast<uintptr_t>(object)
                   << std::dec << '\n';
            std::vector<MonoClass*> hierarchy;
            for (MonoClass* current = actualClass; current; current = includeParents ? resolver.ClassParent(current) : nullptr)
                hierarchy.push_back(current);
            for (auto it = hierarchy.rbegin(); it != hierarchy.rend(); ++it)
            {
                if (output.Full()) { output << " "; break; }
                MonoClass* current = *it;
                const auto fields = resolver.EnumerateFields(current);
                size_t count = 0;
                for (auto* field : fields)
                    if ((resolver.FieldFlags(field) & mono_metadata::FIELD_ATTRIBUTE_STATIC) == 0) ++count;
                output << FullTypeName(current) << " (" << count << " fields):\n";
                for (MonoClassField* field : fields)
                {
                    if (output.Full()) { output << " "; break; }
                    if ((resolver.FieldFlags(field) & mono_metadata::FIELD_ATTRIBUTE_STATIC) != 0) continue;
                    output << "  " << resolver.TypeDisplayName(resolver.FieldType(field)) << ' '
                           << (resolver.FieldName(field) ? resolver.FieldName(field) : "?") << " = ";
                    std::string error;
                    if (LuaBridge_ReadFieldValue(state, LuaBridge_GetInstanceObject(state, 1), field, false,
                                                 error))
                    {
                        std::string value;
                        if (LuaBridge_TryToString(state, -1, value))
                        {
                            const bool quote = lua_type(state, -1) == LUA_TSTRING;
                            output << (quote ? "\"" : "") << value << (quote ? "\"" : "");
                        }
                        else
                            output << "<tostring error: " << value << ">";
                        lua_pop(state, 1);
                    }
                    else
                        output << "<" << error << ">";
                    output << '\n';
                }
                if (!includeParents) break;
                output << '\n';
            }
        }
        std::string text = output.str();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        lua_getglobal(state, "print");
        lua_pushlstring(state, text.data(), text.size());
        lua_call(state, 1, 0);
        return 0;
    }
} // namespace

const luaL_Reg* LuaBinding_GetInstanceMethods()
{
    static const luaL_Reg methods[] = {{"call", Instance_Call},
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
                                       {nullptr, nullptr}};
    return methods;
}
