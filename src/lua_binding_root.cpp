#include "lua_engine.h"
/**
 * lua_binding_root.cpp — lua 与 mono 全局入口
 * 注册通用 Lua 辅助函数、Mono 程序集/类型入口、危险地址包装、Hook 与
 * Unity 主线程调度接口。全局入口只组织调用，不复制运行时或封送逻辑。
 */
#include "lua_binding_internal.h"
#include "mono_resolver.h"
#include "mono_hook.h"
#include "mono_session.h"
#include "mono_scheduler.h"
#include "lua_dump.h"
#include "pipe_channel.h"
extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    bool IsReadableObjectAddress(uintptr_t address)
    {
        if (address < 0x10000 || (address % alignof(void*)) != 0) return false;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) == 0) return false;
        if (memory.State != MEM_COMMIT) return false;
        if ((memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        return regionEnd >= sizeof(void*) * 2 && address <= regionEnd - sizeof(void*) * 2;
    }

    int EachTable(lua_State* state, int table, int callback)
    {
        table = lua_absindex(state, table);
        callback = lua_absindex(state, callback);
        const lua_Integer length = static_cast<lua_Integer>(lua_rawlen(state, table));
        for (lua_Integer index = 1; index <= length; ++index)
        {
            lua_rawgeti(state, table, index);
            if (!lua_isnil(state, -1))
            {
                lua_pushvalue(state, callback);
                lua_pushvalue(state, -2);
                lua_pushinteger(state, index);
                if (lua_pcall(state, 2, 0, 0) != LUA_OK) return lua_error(state);
            }
            lua_pop(state, 1);
        }
        lua_pushnil(state);
        while (lua_next(state, table) != 0)
        {
            const bool visited = lua_isinteger(state, -2) && lua_tointeger(state, -2) >= 1 &&
                                 lua_tointeger(state, -2) <= length;
            if (!visited)
            {
                lua_pushvalue(state, callback);
                lua_pushvalue(state, -2);
                lua_pushvalue(state, -4);
                if (lua_pcall(state, 2, 0, 0) != LUA_OK) return lua_error(state);
            }
            lua_pop(state, 1);
        }
        return 0;
    }

    int Lua_Each(lua_State* state)
    {
        luaL_checktype(state, 1, LUA_TTABLE);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        return EachTable(state, 1, 2);
    }

    int DumpEntry(lua_State* state)
    {
        auto& text = *static_cast<LuaDumpStream*>(lua_touserdata(state, lua_upvalueindex(1)));
        if (text.Full()) { text << " "; return 0; }
        size_t length = 0;
        const char* key = luaL_tolstring(state, 2, &length);
        text << '[';
        text.write(key, static_cast<std::streamsize>(length));
        text << "] = ";
        const char* value = luaL_tolstring(state, 1, &length);
        text.write(value, static_cast<std::streamsize>(length));
        text << '\n';
        return 0;
    }

    int Lua_Dump(lua_State* state)
    {
        luaL_checktype(state, 1, LUA_TTABLE);
        lua_Integer count = 0;
        lua_pushnil(state);
        while (lua_next(state, 1) != 0)
        {
            ++count;
            lua_pop(state, 1);
        }
        LuaDumpStream output;
        output << "count: " << count << '\n';
        lua_pushlightuserdata(state, &output);
        lua_pushcclosure(state, DumpEntry, 1);
        EachTable(state, 1, lua_gettop(state));
        std::string text = output.str();
        if (!text.empty() && text.back() == '\n') text.pop_back();
        lua_getglobal(state, "print");
        lua_pushlstring(state, text.data(), text.size());
        lua_call(state, 1, 0);
        return 0;
    }

    int Lua_Hex(lua_State* state)
    {
        if (!lua_isinteger(state, 1) && !lua_islightuserdata(state, 1))
            return LuaEngine::RaiseBridgeError(state, "hex requires an integer or lightuserdata");
        const uint64_t value = lua_islightuserdata(state, 1)
                                   ? reinterpret_cast<uintptr_t>(lua_touserdata(state, 1))
                                   : static_cast<uint64_t>(luaL_checkinteger(state, 1));
        char text[32]{};
        sprintf_s(text, "0x%llX", static_cast<unsigned long long>(value));
        lua_pushstring(state, text);
        return 1;
    }

    int Mono_GetStatus(lua_State* state)
    {
        // 状态查询只读取快照，不触发跨模块失效。
        std::string status = MonoRuntime::Instance().Status();
        // 与 Il2Cpp 后端保持一致；线程身份仍由独立探针确认。
        status += "\nMain thread: ";
        status += MonoScheduler::IsReady() ? "ready" : "not ready";
        status += "\nRejected log batches: " + std::to_string(PipeChannel::Instance().DroppedLogs());
        lua_pushlstring(state, status.data(), status.size());
        return 1;
    }

    int Mono_GetMissingExports(lua_State* state)
    {
        const auto missing = MonoResolver::Instance().MissingOptionalExports();
        lua_createtable(state, static_cast<int>(missing.size()), 0);
        for (size_t i = 0; i < missing.size(); ++i)
        {
            lua_pushstring(state, missing[i].c_str());
            lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    int Mono_IsInitialized(lua_State* state)
    {
        lua_pushboolean(state, MonoRuntime::Instance().IsInitialized());
        return 1;
    }

    int Mono_GetAssemblies(lua_State* state)
    {
        if (!MonoSession::RefreshAssemblies()) return LuaEngine::RaiseBridgeError(state, "failed to refresh Mono assemblies");
        const auto assemblies = MonoRuntime::Instance().Assemblies();
        lua_createtable(state, static_cast<int>(assemblies.size()), 0);
        for (size_t index = 0; index < assemblies.size(); ++index)
        {
            LuaBridge_PushAssembly(state, assemblies[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        return 1;
    }

    int Mono_GetAssembly(lua_State* state)
    {
        const char* name = luaL_checkstring(state, 1);
        if (!MonoSession::RefreshAssemblies()) return LuaEngine::RaiseBridgeError(state, "failed to refresh Mono assemblies");
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
        const char* nameSpace = luaL_optstring(state, 1, "");
        const char* name = luaL_checkstring(state, 2);
        if (!MonoSession::RefreshAssemblies()) return LuaEngine::RaiseBridgeError(state, "failed to refresh Mono assemblies");
        LuaBridge_PushClass(state, MonoRuntime::Instance().FindClass(nameSpace, name));
        return 1;
    }

    int Mono_Wrap(lua_State* state)
    {
        const uintptr_t rawAddress = lua_islightuserdata(state, 1)
                                         ? reinterpret_cast<uintptr_t>(lua_touserdata(state, 1))
                                         : static_cast<uintptr_t>(luaL_checkinteger(state, 1));
        auto invalid = [state](const char* error) {
            lua_pushnil(state);
            lua_pushstring(state, error);
            return 2;
        };
        if (!IsReadableObjectAddress(rawAddress)) return invalid("invalid or unreadable Mono object address");

        MonoObject* object = reinterpret_cast<MonoObject*>(static_cast<uintptr_t>(rawAddress));
        if (!MonoResolver::Instance().CanValidateObjectHeader())
            return invalid("this Mono runtime cannot validate object headers");
        if (!MonoResolver::Instance().IsValidObjectHeader(object))
            return invalid("address does not contain a valid Mono object header");
        if (!MonoResolver::Instance().ObjectClass(object))
            return invalid("address is not a valid Mono object");

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
        if (!MonoScheduler::Schedule(state, 1)) return LuaEngine::RaiseBridgeError(state, "failed to schedule callback");
        return 0;
    }

    int Mono_SetTick(lua_State* state)
    {
        const LuaMethodUD* method = LuaBridge_CheckMethod(state, 1);
        std::string error;
        const bool success = MonoScheduler::SetTick(method->method, error);
        lua_pushboolean(state, success);
        if (success) return 1;
        lua_pushlstring(state, error.data(), error.size());
        return 2;
    }

    int Mono_GetTick(lua_State* state)
    {
        if (MonoMethod* method = MonoScheduler::GetTick())
        {
            const std::string signature = MonoResolver::Instance().MethodSignature(method);
            lua_pushlstring(state, signature.data(), signature.size());
        }
        else
            lua_pushnil(state);
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
} // namespace

void LuaBinding_RegisterGlobals(lua_State* state)
{
    static const luaL_Reg luaFunctions[] = {
        {"each", Lua_Each}, {"dump", Lua_Dump}, {"hex", Lua_Hex}, {nullptr, nullptr}};

    static const luaL_Reg monoFunctions[] = {{"get_status", Mono_GetStatus},
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
                                             {nullptr, nullptr}};

    RegisterGlobalTable(state, "lua", luaFunctions);
    RegisterGlobalTable(state, "mono", monoFunctions);
}
