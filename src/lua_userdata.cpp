/**
 * ============================================================
 * lua_userdata.cpp — Mono 元数据 userdata 创建与校验
 * ============================================================
 * Assembly/Class/Method/Field 保存带 Runtime generation 的借用元数据指针，
 * Domain/程序集快照改变后旧 userdata 会拒绝访问。Instance 只保存强
 * GCHandle，每次使用重新解析当前对象地址，__gc 与 Runtime 关闭负责释放。
 * ============================================================
 */
#include "lua_bridge.h"
#include "mono_resolver.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

void LuaBridge_PushAssembly(lua_State* state, const MonoAssemblyInfo& assembly)
{
    auto* userdata = static_cast<LuaAssemblyUD*>(
        lua_newuserdatauv(state, sizeof(LuaAssemblyUD), 0));
    userdata->assembly = assembly.assembly;
    userdata->image = assembly.image;
    userdata->generation = assembly.generation;
    luaL_setmetatable(state, LuaBridgeMT::ASSEMBLY);
}

LuaAssemblyUD* LuaBridge_CheckAssembly(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaAssemblyUD*>(
        luaL_checkudata(state, index, LuaBridgeMT::ASSEMBLY));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        luaL_error(state, "Assembly belongs to an expired Mono metadata generation");
    return userdata;
}

void LuaBridge_PushClass(lua_State* state, MonoClass* klass)
{
    if (klass == nullptr)
    {
        lua_pushnil(state);
        return;
    }

    auto* userdata = static_cast<LuaClassUD*>(
        lua_newuserdatauv(state, sizeof(LuaClassUD), 0));
    userdata->klass = klass;
    userdata->generation = MonoRuntime::Instance().Generation();
    luaL_setmetatable(state, LuaBridgeMT::CLASS);
}

LuaClassUD* LuaBridge_CheckClass(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaClassUD*>(
        luaL_checkudata(state, index, LuaBridgeMT::CLASS));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        luaL_error(state, "Class belongs to an expired Mono metadata generation");
    return userdata;
}

void LuaBridge_PushMethod(lua_State* state, MonoMethod* method)
{
    if (!method) { lua_pushnil(state); return; }
    auto* userdata = static_cast<LuaMethodUD*>(
        lua_newuserdatauv(state, sizeof(LuaMethodUD), 0));
    userdata->method = method;
    userdata->generation = MonoRuntime::Instance().Generation();
    luaL_setmetatable(state, LuaBridgeMT::METHOD);
}

LuaMethodUD* LuaBridge_CheckMethod(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaMethodUD*>(
        luaL_checkudata(state, index, LuaBridgeMT::METHOD));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        luaL_error(state, "Method belongs to an expired Mono metadata generation");
    return userdata;
}

void LuaBridge_PushField(lua_State* state, MonoClassField* field)
{
    if (!field) { lua_pushnil(state); return; }
    auto* userdata = static_cast<LuaFieldUD*>(
        lua_newuserdatauv(state, sizeof(LuaFieldUD), 0));
    userdata->field = field;
    userdata->generation = MonoRuntime::Instance().Generation();
    luaL_setmetatable(state, LuaBridgeMT::FIELD);
}

LuaFieldUD* LuaBridge_CheckField(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaFieldUD*>(
        luaL_checkudata(state, index, LuaBridgeMT::FIELD));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        luaL_error(state, "Field belongs to an expired Mono metadata generation");
    return userdata;
}

void LuaBridge_PushInstance(lua_State* state, MonoObject* object)
{
    if (!object)
    {
        lua_pushnil(state);
        return;
    }

    const uint32_t handle = MonoResolver::Instance().CreateGCHandle(object);
    if (!handle)
    {
        luaL_error(state, "failed to create Mono GC handle");
        return;
    }

    LuaBridge_PushInstanceHandle(state, handle);
}

void LuaBridge_PushInstanceHandle(lua_State* state, uint32_t handle)
{
    if (!handle)
    {
        lua_pushnil(state);
        return;
    }
    auto* userdata = static_cast<LuaInstanceUD*>(
        lua_newuserdatauv(state, sizeof(LuaInstanceUD), 0));
    userdata->gcHandle = handle;
    userdata->generation = MonoRuntime::Instance().Generation();
    luaL_setmetatable(state, LuaBridgeMT::INSTANCE);
}

LuaInstanceUD* LuaBridge_CheckInstance(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaInstanceUD*>(
        luaL_checkudata(state, index, LuaBridgeMT::INSTANCE));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        luaL_error(state, "Instance belongs to an expired Mono metadata generation");
    return userdata;
}

MonoObject* LuaBridge_GetInstanceObject(lua_State* state, int index)
{
    const LuaInstanceUD* userdata = LuaBridge_CheckInstance(state, index);
    MonoObject* object = MonoResolver::Instance().GCHandleTarget(userdata->gcHandle);
    if (!object) luaL_error(state, "Mono object is no longer available");
    return object;
}
