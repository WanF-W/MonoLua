/**
 * ============================================================
 * lua_bridge.h — Lua ↔ Mono 桥接层公共声明
 * ============================================================
 * 定义五类 userdata、metadata generation 和桥接注册入口。Binding 按
 * 初始化、全局入口、各 userdata、值转换和容器分别实现，避免形成单一
 * 巨型源文件。
 *
 * ·Assembly/Class/Method/Field 保存 Mono 元数据借用指针和 generation
 * ·Instance 保存强 GCHandle，不把可移动 MonoObject* 作为长期状态
 * ·所有公共操作通过独立元表暴露，命名与 Il2CppLua 保持一致
 * ============================================================
 */
#pragma once
#include "mono_runtime.h"

struct lua_State;
struct luaL_Reg;

namespace LuaBridgeMT
{
    inline constexpr const char* ASSEMBLY = "MonoLua.Assembly";
    inline constexpr const char* CLASS = "MonoLua.Class";
    inline constexpr const char* INSTANCE = "MonoLua.Instance";
    inline constexpr const char* METHOD = "MonoLua.Method";
    inline constexpr const char* FIELD = "MonoLua.Field";
}

struct LuaAssemblyUD
{
    MonoAssembly* assembly = nullptr;
    MonoImage* image = nullptr;
    uint64_t generation = 0;
};

struct LuaClassUD
{
    MonoClass* klass = nullptr;
    uint64_t generation = 0;
};

struct LuaMethodUD
{
    MonoMethod* method = nullptr;
    uint64_t generation = 0;
};

struct LuaFieldUD
{
    MonoClassField* field = nullptr;
    uint64_t generation = 0;
};

struct LuaInstanceUD
{
    uint32_t gcHandle = 0;
    uint64_t generation = 0;
};

bool LuaBridge_Init(lua_State* state);
void LuaBinding_RegisterGlobals(lua_State* state);
const luaL_Reg* LuaBinding_GetAssemblyMethods();
const luaL_Reg* LuaBinding_GetClassMethods();
const luaL_Reg* LuaBinding_GetMethodMethods();
const luaL_Reg* LuaBinding_GetFieldMethods();
const luaL_Reg* LuaBinding_GetInstanceMethods();
void LuaBridge_PushAssembly(lua_State* state, const MonoAssemblyInfo& assembly);
LuaAssemblyUD* LuaBridge_CheckAssembly(lua_State* state, int index);
void LuaBridge_PushClass(lua_State* state, MonoClass* klass);
LuaClassUD* LuaBridge_CheckClass(lua_State* state, int index);
void LuaBridge_PushMethod(lua_State* state, MonoMethod* method);
LuaMethodUD* LuaBridge_CheckMethod(lua_State* state, int index);
void LuaBridge_PushField(lua_State* state, MonoClassField* field);
LuaFieldUD* LuaBridge_CheckField(lua_State* state, int index);
void LuaBridge_PushInstance(lua_State* state, MonoObject* object);
void LuaBridge_PushInstanceHandle(lua_State* state, uint32_t handle);
LuaInstanceUD* LuaBridge_CheckInstance(lua_State* state, int index);
MonoObject* LuaBridge_GetInstanceObject(lua_State* state, int index);
