#include "lua_engine.h"
// 元数据 userdata 借用当前快照；Instance 用强 GCHandle 保存对象。
// 校验不刷新程序集。显式程序集查询发现移除时推进 generation；不支持热重载。
#include "lua_bridge.h"
#include "mono_resolver.h"
#include "mono_handle.h"
extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    int ProtectedToString(lua_State* state)
    {
        luaL_checkany(state, 1);
        luaL_tolstring(state, 1, nullptr);
        return 1;
    }
} // namespace

bool LuaBridge_TryToString(lua_State* state, int index, std::string& text)
{
    text.clear();
    const int absoluteIndex = lua_absindex(state, index);
    lua_pushcfunction(state, ProtectedToString);
    lua_pushvalue(state, absoluteIndex);
    const int status = lua_pcall(state, 1, 1, 0);
    size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    if (value) text.assign(value, length);
    else if (status != LUA_OK) text = LuaEngine::ErrorText(state, -1);
    lua_pop(state, 1);
    return status == LUA_OK;
}

void LuaBridge_PushAssembly(lua_State* state, const MonoAssemblyInfo& assembly)
{
    auto* userdata = static_cast<LuaAssemblyUD*>(lua_newuserdatauv(state, sizeof(LuaAssemblyUD), 0));
    userdata->assembly = assembly.assembly;
    userdata->image = assembly.image;
    userdata->generation = assembly.generation;
    luaL_setmetatable(state, LuaBridgeMT::ASSEMBLY);
}

LuaAssemblyUD* LuaBridge_CheckAssembly(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaAssemblyUD*>(luaL_checkudata(state, index, LuaBridgeMT::ASSEMBLY));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        LuaEngine::RaiseBridgeError(state, "Assembly belongs to an expired Mono metadata generation");
    return userdata;
}

void LuaBridge_PushClass(lua_State* state, MonoClass* klass)
{
    if (klass == nullptr)
    {
        lua_pushnil(state);
        return;
    }

    auto* userdata = static_cast<LuaClassUD*>(lua_newuserdatauv(state, sizeof(LuaClassUD), 0));
    userdata->klass = klass;
    userdata->generation = MonoRuntime::Instance().Generation();
    luaL_setmetatable(state, LuaBridgeMT::CLASS);
}

LuaClassUD* LuaBridge_CheckClass(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaClassUD*>(luaL_checkudata(state, index, LuaBridgeMT::CLASS));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        LuaEngine::RaiseBridgeError(state, "Class belongs to an expired Mono metadata generation");
    return userdata;
}

void LuaBridge_PushMethod(lua_State* state, MonoMethod* method)
{
    if (!method)
    {
        lua_pushnil(state);
        return;
    }
    auto* userdata = static_cast<LuaMethodUD*>(lua_newuserdatauv(state, sizeof(LuaMethodUD), 0));
    userdata->method = method;
    userdata->generation = MonoRuntime::Instance().Generation();
    luaL_setmetatable(state, LuaBridgeMT::METHOD);
}

LuaMethodUD* LuaBridge_CheckMethod(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaMethodUD*>(luaL_checkudata(state, index, LuaBridgeMT::METHOD));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        LuaEngine::RaiseBridgeError(state, "Method belongs to an expired Mono metadata generation");
    return userdata;
}

void LuaBridge_PushField(lua_State* state, MonoClassField* field)
{
    if (!field)
    {
        lua_pushnil(state);
        return;
    }
    auto* userdata = static_cast<LuaFieldUD*>(lua_newuserdatauv(state, sizeof(LuaFieldUD), 0));
    userdata->field = field;
    userdata->generation = MonoRuntime::Instance().Generation();
    luaL_setmetatable(state, LuaBridgeMT::FIELD);
}

LuaFieldUD* LuaBridge_CheckField(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaFieldUD*>(luaL_checkudata(state, index, LuaBridgeMT::FIELD));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        LuaEngine::RaiseBridgeError(state, "Field belongs to an expired Mono metadata generation");
    return userdata;
}

void LuaBridge_PushInstance(lua_State* state, MonoObject* object)
{
    std::string error;
    if (!LuaBridge_TryPushInstance(state, object, error)) LuaEngine::RaiseBridgeError(state, "%s", error.c_str());
}

bool LuaBridge_TryPushInstance(lua_State* state, MonoObject* object, std::string& error)
{
    error.clear();
    if (!object)
    {
        lua_pushnil(state);
        return true;
    }

    auto& resolver = MonoResolver::Instance();
    mono::ScopedGCHandle handle(resolver, resolver.CreateGCHandle(object));
    if (!handle.value)
    {
        error = "failed to create Mono GC handle";
        return false;
    }

    LuaBridge_PushInstanceHandle(state, handle.value);
    return true;
}

void LuaBridge_PushInstanceHandle(lua_State* state, MonoGCHandle& handle)
{
    if (!handle)
    {
        lua_pushnil(state);
        return;
    }
    auto* userdata = static_cast<LuaInstanceUD*>(lua_newuserdatauv(state, sizeof(LuaInstanceUD), 0));
    userdata->gcHandle = 0;
    userdata->generation = MonoRuntime::Instance().Generation();
    luaL_setmetatable(state, LuaBridgeMT::INSTANCE);
    userdata->gcHandle = std::exchange(handle, 0);
}

LuaInstanceUD* LuaBridge_CheckInstance(lua_State* state, int index)
{
    auto* userdata = static_cast<LuaInstanceUD*>(luaL_checkudata(state, index, LuaBridgeMT::INSTANCE));
    if (userdata->generation != MonoRuntime::Instance().Generation())
        LuaEngine::RaiseBridgeError(state, "Instance belongs to an expired Mono metadata generation");
    return userdata;
}

MonoObject* LuaBridge_GetInstanceObject(lua_State* state, int index)
{
    const LuaInstanceUD* userdata = LuaBridge_CheckInstance(state, index);
    MonoObject* object = MonoResolver::Instance().GCHandleTarget(userdata->gcHandle);
    if (!object) LuaEngine::RaiseBridgeError(state, "Mono object is no longer available");
    return object;
}

bool LuaBridge_TryGetInstanceObject(lua_State* state, int index, MonoObject*& object)
{
    object = nullptr;
    auto* userdata = static_cast<LuaInstanceUD*>(luaL_testudata(state, index, LuaBridgeMT::INSTANCE));
    if (!userdata || userdata->generation != MonoRuntime::Instance().Generation()) return false;
    object = MonoResolver::Instance().GCHandleTarget(userdata->gcHandle);
    return object != nullptr;
}
