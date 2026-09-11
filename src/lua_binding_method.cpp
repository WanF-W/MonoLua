#include "lua_engine.h"
/**
 * lua_binding_method.cpp — Method userdata 公开 API
 * 公开方法元数据、显式反射调用、JIT 地址查询以及 Hook 生命周期管理。
 * 调用统一经过 lua_method_call 参数编组与 mono_runtime_invoke。
 */
#include "lua_binding_internal.h"
#include "mono_metadata.h"
#include "mono_resolver.h"
#include "mono_hook.h"

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    int Method_GetName(lua_State* state)
    {
        const LuaMethodUD* userdata = LuaBridge_CheckMethod(state, 1);
        const char* name = MonoResolver::Instance().MethodName(userdata->method);
        lua_pushstring(state, name ? name : "");
        return 1;
    }

    int Method_GetClass(lua_State* state)
    {
        const LuaMethodUD* userdata = LuaBridge_CheckMethod(state, 1);
        LuaBridge_PushClass(state, MonoResolver::Instance().MethodClass(userdata->method));
        return 1;
    }

    int Method_GetSignature(lua_State* state)
    {
        const LuaMethodUD* userdata = LuaBridge_CheckMethod(state, 1);
        const std::string signature = MonoResolver::Instance().MethodSignature(userdata->method);
        lua_pushlstring(state, signature.data(), signature.size());
        return 1;
    }

    int Method_ToString(lua_State* state)
    {
        const auto* userdata = LuaBridge_CheckMethod(state, 1);
        auto& resolver = MonoResolver::Instance();
        const std::string signature = resolver.MethodSignature(userdata->method);
        lua_pushfstring(state, "Method: %s @ %p", signature.c_str(), resolver.CompileMethod(userdata->method));
        return 1;
    }

    int Method_Call(lua_State* state)
    {
        const LuaMethodUD* userdata = LuaBridge_CheckMethod(state, 1);
        auto& resolver = MonoResolver::Instance();
        MonoObject* object = nullptr;
        int firstArgument = 2;

        if ((resolver.MethodFlags(userdata->method) & mono_metadata::METHOD_ATTRIBUTE_STATIC) == 0)
        {
            object = LuaBridge_GetInstanceObject(state, 2);
            if (!resolver.ObjectIsInstanceOf(object, resolver.MethodClass(userdata->method)))
                return LuaEngine::RaiseBridgeError(state, "instance is not compatible with the method's declaring class");
            firstArgument = 3;
        }
        return LuaBridge_InvokeMethod(state, userdata->method, object, firstArgument);
    }

    int Method_GetAddress(lua_State* state)
    {
        const LuaMethodUD* userdata = LuaBridge_CheckMethod(state, 1);
        void* address = MonoResolver::Instance().CompileMethod(userdata->method);
        if (!address)
        {
            lua_pushnil(state);
            return 1;
        }
        lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(address)));
        return 1;
    }

    int Method_Hook(lua_State* state)
    {
        const LuaMethodUD* userdata = LuaBridge_CheckMethod(state, 1);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        std::string error;
        if (!MonoHook::HookMethod(state, userdata->method, 2, error))
            return LuaEngine::RaiseBridgeError(state, "%s", error.c_str());
        return 0;
    }

    int Method_IsHooked(lua_State* state)
    {
        const LuaMethodUD* userdata = LuaBridge_CheckMethod(state, 1);
        lua_pushboolean(state, MonoHook::IsHooked(userdata->method));
        return 1;
    }

    int Method_Unhook(lua_State* state)
    {
        const LuaMethodUD* userdata = LuaBridge_CheckMethod(state, 1);
        if (!MonoHook::UnhookMethod(userdata->method)) return LuaEngine::RaiseBridgeError(state, "method is not hooked");
        return 0;
    }
} // namespace

const luaL_Reg* LuaBinding_GetMethodMethods()
{
    static const luaL_Reg methods[] = {{"get_name", Method_GetName},           {"get_class", Method_GetClass},
                                       {"get_signature", Method_GetSignature}, {"call", Method_Call},
                                       {"get_address", Method_GetAddress},     {"hook", Method_Hook},
                                       {"is_hooked", Method_IsHooked},         {"unhook", Method_Unhook},
                                       {"__tostring", Method_ToString},        {nullptr, nullptr}};
    return methods;
}
