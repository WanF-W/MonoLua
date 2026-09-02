/**
 * ============================================================
 * lua_binding_internal.h — Binding 实现间的内部声明
 * ============================================================
 * 只暴露 Binding 拆分文件共享的值转换、重载评分和容器函数，不作为
 * Lua 公共 API，也不保存运行时状态或托管对象所有权。
 * ============================================================
 */
#pragma once
#include "lua_bridge.h"

// 本头文件的公开声明直接使用 lua_Integer。仅前置声明 lua_State 不足以
// 提供该整数别名，且会让后续函数声明被编译器错误恢复成其他签名。
extern "C" {
#include "lua.h"
}

void LuaBridge_PushAssemblyTable(lua_State* state);
bool LuaBridge_ReadFieldValue(
    lua_State* state, MonoObject* object, MonoClassField* field,
    bool isStatic, std::string& error);
bool LuaBridge_WriteFieldValue(
    lua_State* state, int valueIndex, MonoObject* object,
    MonoClassField* field, bool isStatic, std::string& error);
bool LuaBridge_CanMarshalMethodArguments(
    lua_State* state, int firstArgument, const std::vector<MonoType*>& types);
int LuaBridge_ScoreMethodArguments(
    lua_State* state, int firstArgument, const std::vector<MonoType*>& types);
int LuaBridge_InvokeMethod(
    lua_State* state, MonoMethod* method, MonoObject* object, int firstArgument);
bool LuaBridge_PushRawValue(
    lua_State* state, MonoType* type, void* address, std::string& error);
bool LuaBridge_WriteArrayValue(
    lua_State* state, int valueIndex, MonoArray* array, MonoType* type,
    uintptr_t index, std::string& error);
bool LuaBridge_IsArray(MonoObject* object);
bool LuaBridge_IsList(MonoObject* object);
int LuaBridge_ContainerLength(lua_State* state, MonoObject* object);
int LuaBridge_ContainerIndex(lua_State* state, MonoObject* object, lua_Integer index);
int LuaBridge_ContainerNewIndex(
    lua_State* state, MonoObject* object, lua_Integer index, int valueIndex);
