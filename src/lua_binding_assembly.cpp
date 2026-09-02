/**
 * ============================================================
 * lua_binding_assembly.cpp — Assembly userdata 公开 API
 * ============================================================
 * Assembly userdata 同时保存 MonoAssembly 与对应 MonoImage 借用指针。
 * 类型查找和枚举通过 Image 完成；每次访问先校验 Runtime metadata generation。
 * ============================================================
 */
#include "lua_bridge.h"
#include "mono_resolver.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    int Assembly_GetName(lua_State* state)
    {
        const LuaAssemblyUD* assembly = LuaBridge_CheckAssembly(state, 1);
        const char* name = MonoResolver::Instance().ImageName(assembly->image);
        if (name == nullptr)
        {
            lua_pushnil(state);
            return 1;
        }

        lua_pushstring(state, name);
        return 1;
    }

    int Assembly_ToString(lua_State* state)
    {
        const LuaAssemblyUD* assembly = LuaBridge_CheckAssembly(state, 1);
        const char* name = MonoResolver::Instance().ImageName(assembly->image);
        char text[256]{};
        sprintf_s(text, "Assembly: %s @ 0x%llX",
            name != nullptr ? name : "<invalid>",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(assembly->assembly)));
        lua_pushstring(state, text);
        return 1;
    }

    int Assembly_GetClass(lua_State* state)
    {
        const LuaAssemblyUD* assembly = LuaBridge_CheckAssembly(state, 1);
        const char* nameSpace = luaL_checkstring(state, 2);
        const char* name = luaL_checkstring(state, 3);
        LuaBridge_PushClass(
            state,
            MonoResolver::Instance().FindClass(assembly->image, nameSpace, name));
        return 1;
    }

    int Assembly_GetClasses(lua_State* state)
    {
        const LuaAssemblyUD* assembly = LuaBridge_CheckAssembly(state, 1);
        const auto classes = MonoResolver::Instance().EnumerateClasses(assembly->image);
        lua_createtable(state, static_cast<int>(classes.size()), 0);
        for (size_t index = 0; index < classes.size(); ++index)
        {
            LuaBridge_PushClass(state, classes[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        return 1;
    }
}

const luaL_Reg* LuaBinding_GetAssemblyMethods()
{
    static const luaL_Reg methods[] = {
        {"get_name", Assembly_GetName},
        {"get_class", Assembly_GetClass},
        {"get_classes", Assembly_GetClasses},
        {"__tostring", Assembly_ToString},
        {nullptr, nullptr}
    };
    return methods;
}
