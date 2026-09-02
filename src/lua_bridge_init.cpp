/**
 * ============================================================
 * lua_bridge_init.cpp — Lua 元表与全局模块注册
 * ============================================================
 * 按 Assembly/Class/Method/Field/Instance 分别建立元表。Instance 自定义
 * __index 以支持数组和 List 索引，其余 userdata 直接以元表作为方法表。
 * ============================================================
 */
#include "lua_bridge.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    void CreateMetatable(lua_State* state, const char* name, const luaL_Reg* methods)
    {
        luaL_newmetatable(state, name);
        luaL_setfuncs(state, methods, 0);
        // Instance 自己实现数字索引；其他 userdata 没有自定义 __index 时，
        // 才把元表本身作为方法表。
        lua_getfield(state, -1, "__index");
        const bool hasCustomIndex = !lua_isnil(state, -1);
        lua_pop(state, 1);
        if (!hasCustomIndex)
        {
            lua_pushvalue(state, -1);
            lua_setfield(state, -2, "__index");
        }
        lua_pop(state, 1);
    }
}

bool LuaBridge_Init(lua_State* state)
{
    if (state == nullptr) return false;
    CreateMetatable(state, LuaBridgeMT::ASSEMBLY, LuaBinding_GetAssemblyMethods());
    CreateMetatable(state, LuaBridgeMT::CLASS, LuaBinding_GetClassMethods());
    CreateMetatable(state, LuaBridgeMT::METHOD, LuaBinding_GetMethodMethods());
    CreateMetatable(state, LuaBridgeMT::FIELD, LuaBinding_GetFieldMethods());
    CreateMetatable(state, LuaBridgeMT::INSTANCE, LuaBinding_GetInstanceMethods());
    LuaBinding_RegisterGlobals(state);
    return true;
}
