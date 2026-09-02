/**
 * ============================================================
 * lua_binding_root.cpp — lua 与 mono 全局入口
 * ============================================================
 * 注册通用 Lua 辅助函数、Mono 程序集/类型入口、危险地址包装、Hook 与
 * Unity 主线程调度接口。全局入口只组织调用，不复制运行时或封送逻辑。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include "mono_resolver.h"
#include "mono_hook.h"
#include "mono_scheduler.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    bool IsReadableObjectAddress(uintptr_t address)
    {
        if (address < 0x10000 || (address % alignof(void*)) != 0) return false;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) == 0)
            return false;
        if (memory.State != MEM_COMMIT) return false;
        if ((memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        return regionEnd >= sizeof(void*) * 2 && address <= regionEnd - sizeof(void*) * 2;
    }

    int Lua_Each(lua_State* state)
    {
        luaL_checktype(state, 1, LUA_TTABLE);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        lua_pushnil(state);
        while (lua_next(state, 1) != 0)
        {
            lua_pushvalue(state, 2);
            lua_pushvalue(state, -2);
            lua_pushvalue(state, -4);
            lua_call(state, 2, 0);
            lua_pop(state, 1);
        }
        return 0;
    }

    int Lua_Dump(lua_State* state)
    {
        luaL_checktype(state, 1, LUA_TTABLE);
        lua_pushnil(state);
        while (lua_next(state, 1) != 0)
        {
            lua_getglobal(state, "print");
            lua_pushvalue(state, -3);
            lua_pushvalue(state, -3);
            lua_call(state, 2, 0);
            lua_pop(state, 1);
        }
        return 0;
    }

    int Lua_Hex(lua_State* state)
    {
        const uint64_t value = static_cast<uint64_t>(luaL_checkinteger(state, 1));
        char text[32]{};
        sprintf_s(text, "0x%llX", static_cast<unsigned long long>(value));
        lua_pushstring(state, text);
        return 1;
    }

    int Mono_GetStatus(lua_State* state)
    {
        std::string status = MonoRuntime::Instance().Status();
        status += "\nMain thread: ";
        status += MonoScheduler::IsReady() ? "ready" : "not ready";
        lua_pushlstring(state, status.data(), status.size());
        return 1;
    }

    int Mono_GetMissingExports(lua_State* state)
    {
        const auto missing = MonoResolver::Instance().MissingOptionalExports();
        lua_createtable(state, static_cast<int>(missing.size()), 0);
        for (size_t i = 0; i < missing.size(); ++i)
            lua_pushstring(state, missing[i].c_str()), lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
        return 1;
    }

    int Mono_IsInitialized(lua_State* state)
    {
        lua_pushboolean(state, MonoRuntime::Instance().IsInitialized());
        return 1;
    }

    int Mono_GetAssemblies(lua_State* state)
    {
        MonoRuntime::Instance().RefreshAssemblies();
        LuaBridge_PushAssemblyTable(state);
        return 1;
    }

    int Mono_GetAssembly(lua_State* state)
    {
        const char* name = luaL_checkstring(state, 1);
        MonoRuntime::Instance().RefreshAssemblies();
        MonoAssemblyInfo assembly;
        if (!MonoRuntime::Instance().FindAssembly(name, assembly))
        {
            lua_pushnil(state);
            return 1;
        }
        LuaBridge_PushAssembly(state, assembly);
        return 1;
    }

    int Mono_GetClass(lua_State* state)
    {
        const char* nameSpace = luaL_checkstring(state, 1);
        const char* name = luaL_checkstring(state, 2);
        MonoRuntime::Instance().RefreshAssemblies();
        LuaBridge_PushClass(
            state,
            MonoRuntime::Instance().FindClass(nameSpace, name));
        return 1;
    }

    int Mono_Wrap(lua_State* state)
    {
        const lua_Integer rawAddress = luaL_checkinteger(state, 1);
        if (rawAddress <= 0 || !IsReadableObjectAddress(static_cast<uintptr_t>(rawAddress)))
            return luaL_error(state, "invalid or unreadable Mono object address");

        MonoObject* object = reinterpret_cast<MonoObject*>(static_cast<uintptr_t>(rawAddress));
        if (!MonoResolver::Instance().CanValidateObjectHeader())
            return luaL_error(state, "this Mono runtime cannot validate object headers");
        if (!MonoResolver::Instance().IsValidObjectHeader(object))
            return luaL_error(state, "address does not contain a valid Mono object header");
        if (!MonoResolver::Instance().ObjectClass(object))
            return luaL_error(state, "address is not a valid Mono object");

        LuaBridge_PushInstance(state, object);
        return 1;
    }

    int Mono_UnhookAll(lua_State*)
    {
        MonoHook::UnhookAll();
        return 0;
    }

    int Mono_Schedule(lua_State* state)
    {
        luaL_checktype(state, 1, LUA_TFUNCTION);
        if (!MonoScheduler::Schedule(state, 1)) return luaL_error(state, "failed to schedule callback");
        return 0;
    }

    int Mono_SetTick(lua_State* state)
    {
        const LuaMethodUD* method = LuaBridge_CheckMethod(state, 1);
        std::string error;
        if (!MonoScheduler::SetTick(method->method, error)) return luaL_error(state, "%s", error.c_str());
        return 0;
    }

    int Mono_GetTick(lua_State* state)
    {
        LuaBridge_PushMethod(state, MonoScheduler::GetTick());
        return 1;
    }

    int Mono_IsTickReady(lua_State* state)
    {
        lua_pushboolean(state, MonoScheduler::IsReady());
        return 1;
    }

    void RegisterGlobalTable(lua_State* state, const char* name, const luaL_Reg* functions)
    {
        lua_newtable(state);
        luaL_setfuncs(state, functions, 0);
        lua_setglobal(state, name);
    }
}

void LuaBridge_PushAssemblyTable(lua_State* state)
{
    const auto assemblies = MonoRuntime::Instance().Assemblies();
    lua_createtable(state, static_cast<int>(assemblies.size()), 0);
    int index = 1;
    for (const MonoAssemblyInfo& assembly : assemblies)
    {
        LuaBridge_PushAssembly(state, assembly);
        lua_rawseti(state, -2, index++);
    }
}

void LuaBinding_RegisterGlobals(lua_State* state)
{
    static const luaL_Reg luaFunctions[] = {
        {"each", Lua_Each},
        {"dump", Lua_Dump},
        {"hex", Lua_Hex},
        {nullptr, nullptr}
    };

    static const luaL_Reg monoFunctions[] = {
        {"get_status", Mono_GetStatus},
        {"get_missing_exports", Mono_GetMissingExports},
        {"is_initialized", Mono_IsInitialized},
        {"get_assemblies", Mono_GetAssemblies},
        {"get_assembly", Mono_GetAssembly},
        {"get_class", Mono_GetClass},
        {"wrap", Mono_Wrap},
        {"unhook_all", Mono_UnhookAll},
        {"schedule", Mono_Schedule},
        {"set_tick", Mono_SetTick},
        {"get_tick", Mono_GetTick},
        {"is_tick_ready", Mono_IsTickReady},
        {nullptr, nullptr}
    };

    RegisterGlobalTable(state, "lua", luaFunctions);
    RegisterGlobalTable(state, "mono", monoFunctions);
}
