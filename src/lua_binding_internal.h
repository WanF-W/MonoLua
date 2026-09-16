/**
 * lua_binding_internal.h — Binding 实现间的内部声明
 * 只暴露 Binding 拆分文件共享的值转换、重载评分和容器函数，不作为
 * Lua 公共 API，也不保存运行时状态或托管对象所有权。
 */
#pragma once
#include "lua_bridge.h"

// 本头文件的公开声明直接使用 lua_Integer。仅前置声明 lua_State 不足以
// 提供该整数别名，且会让后续函数声明被编译器错误恢复成其他签名。
extern "C"
{
#include "lua.h"
}

bool LuaBridge_ReadScalarValue(lua_State* state, int index, int kind, void* storage, std::string& error);
bool LuaBridge_ReadFieldValue(lua_State* state, MonoObject* object, MonoClassField* field, bool isStatic,
                              std::string& error);
bool LuaBridge_WriteFieldValue(lua_State* state, int valueIndex, MonoObject* object, MonoClassField* field,
                               bool isStatic, std::string& error);
MonoMethod* LuaBridge_FindBestMethod(lua_State* state, MonoClass* klass, const char* name, int firstArgument,
                                     bool staticOnly);
// Hook-only invocation state. Captured arguments are already signature-checked native slots;
// handles remain owned by the active callback. A completed result handle transfers to the caller.
struct LuaMethodInvocation
{
    const std::vector<uint64_t>* arguments = nullptr;
    const std::vector<MonoGCHandle>* handles = nullptr;
    bool* entered = nullptr;
    MonoGCHandle* resultHandle = nullptr;
    uint64_t* resultInt = nullptr;
    uint64_t* resultFloat = nullptr;
    void (*beforeInvoke)(void*) = nullptr;
    void* context = nullptr;
};
int LuaBridge_InvokeMethod(lua_State* state, MonoMethod* method, MonoObject* object, int firstArgument,
                          LuaMethodInvocation* invocation = nullptr);
bool LuaBridge_PushRawValue(lua_State* state, MonoType* type, void* address, std::string& error);
bool LuaBridge_WriteArrayValue(lua_State* state, int valueIndex, MonoArray* array, MonoType* type,
                               uintptr_t index, std::string& error);
bool LuaBridge_IsArray(MonoObject* object);
bool LuaBridge_IsList(MonoObject* object);
int LuaBridge_ContainerLength(lua_State* state, MonoObject* object);
// Index adds exactly one result; NewIndex leaves the stack unchanged.
int LuaBridge_ContainerIndex(lua_State* state, MonoObject* object, lua_Integer index);
int LuaBridge_ContainerNewIndex(lua_State* state, MonoObject* object, lua_Integer index, int valueIndex);
