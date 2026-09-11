#include "lua_engine.h"
/**
 * lua_container.cpp — Mono 数组与 List<T> 容器桥接
 * 数组通过 Mono API 操作；List<T> 通过 get_Count/get_Item/set_Item 反射
 * 调用，不把 _items/_size 私有布局作为主实现。
 */
#include "lua_binding_internal.h"
#include "mono_metadata.h"
#include "mono_handle.h"
using mono::ScopedGCHandle;

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    MonoMethod* FindInstanceMethod(MonoClass* klass, const char* name, size_t parameterCount)
    {
        auto& resolver = MonoResolver::Instance();
        for (MonoClass* current = klass; current; current = resolver.ClassParent(current))
        {
            for (MonoMethod* method : resolver.EnumerateMethods(current))
            {
                const char* methodName = resolver.MethodName(method);
                if (methodName && strcmp(methodName, name) == 0 &&
                    (resolver.MethodFlags(method) & mono_metadata::METHOD_ATTRIBUTE_STATIC) == 0 &&
                    resolver.MethodParameters(method).size() == parameterCount)
                    return method;
            }
        }
        return nullptr;
    }
} // namespace

bool LuaBridge_IsArray(MonoObject* object)
{
    if (!object) return false;
    auto& resolver = MonoResolver::Instance();
    const int kind = resolver.TypeKind(resolver.ClassType(resolver.ObjectClass(object)));
    return kind == mono_metadata::TYPE_ARRAY || kind == mono_metadata::TYPE_SZARRAY;
}

bool LuaBridge_IsList(MonoObject* object)
{
    if (!object) return false;
    auto& resolver = MonoResolver::Instance();
    MonoClass* klass = resolver.ObjectClass(object);
    const char* name = resolver.ClassName(klass);
    const char* nameSpace = resolver.ClassNamespace(klass);
    return name && nameSpace && strcmp(name, "List`1") == 0 &&
           strcmp(nameSpace, "System.Collections.Generic") == 0;
}

int LuaBridge_ContainerLength(lua_State* state, MonoObject* object)
{
    if (LuaBridge_IsArray(object))
    {
        auto& resolver = MonoResolver::Instance();
        if (!resolver.CanReadArrays()) LuaEngine::RaiseBridgeError(state, "Mono array reading exports are unavailable");
        MonoClass* klass = resolver.ObjectClass(object);
        const int rank = resolver.TypeKind(resolver.ClassType(klass)) == mono_metadata::TYPE_SZARRAY
                             ? 1
                             : resolver.ClassRank(klass);
        if (rank < 0) LuaEngine::RaiseBridgeError(state, "Mono array rank export is unavailable");
        if (rank != 1) LuaEngine::RaiseBridgeError(state, "multidimensional arrays are not supported");
        const uintptr_t length = resolver.ArrayLength(reinterpret_cast<MonoArray*>(object));
        if (length > static_cast<uintptr_t>(INT32_MAX))
            LuaEngine::RaiseBridgeError(state, "array length exceeds the Lua container limit");
        return static_cast<int>(length);
    }
    if (!LuaBridge_IsList(object)) LuaEngine::RaiseBridgeError(state, "Instance is not an array or List<T>");
    auto& resolver = MonoResolver::Instance();
    MonoMethod* getter = FindInstanceMethod(resolver.ObjectClass(object), "get_Count", 0);
    if (!getter) return LuaEngine::RaiseBridgeError(state, "List<T>.get_Count was not found");
    const int top = lua_gettop(state);
    LuaBridge_InvokeMethod(state, getter, object, top + 1);
    const lua_Integer count = luaL_checkinteger(state, -1);
    lua_settop(state, top);
    if (count < 0 || count > INT32_MAX) return LuaEngine::RaiseBridgeError(state, "List<T>.Count is out of range");
    const int length = static_cast<int>(count);
    return length;
}

int LuaBridge_ContainerIndex(lua_State* state, MonoObject* object, lua_Integer index)
{
    auto& ownerResolver = MonoResolver::Instance();
    ScopedGCHandle owner(ownerResolver, ownerResolver.CreateGCHandle(object, true));
    if (!owner.value) return LuaEngine::RaiseBridgeError(state, "failed to pin container");
    object = owner.Target();
    const int length = LuaBridge_ContainerLength(state, object);
    if (index < 1 || index > length)
    {
        char message[96];
        sprintf_s(message, "container index out of range: %lld", static_cast<long long>(index));
        return LuaEngine::RaiseBridgeError(state, "%s", message);
    }
    if (LuaBridge_IsArray(object))
    {
        auto& resolver = MonoResolver::Instance();
        auto* array = reinterpret_cast<MonoArray*>(owner.Target());
        MonoClass* arrayClass = resolver.ObjectClass(reinterpret_cast<MonoObject*>(array));
        MonoClass* elementClass = resolver.ElementClass(arrayClass);
        MonoType* elementType = resolver.ClassType(elementClass);
        const int elementSize = resolver.ArrayElementSize(arrayClass);
        void* address = resolver.ArrayAddress(array, elementSize, static_cast<uintptr_t>(index - 1));
        std::string error;
        const bool pushed = LuaBridge_PushRawValue(state, elementType, address, error);
        if (!pushed) return LuaEngine::RaiseBridgeError(state, "%s", error.c_str());
        return 1;
    }
    MonoMethod* getter = FindInstanceMethod(MonoResolver::Instance().ObjectClass(object), "get_Item", 1);
    if (!getter) return LuaEngine::RaiseBridgeError(state, "List<T>.get_Item was not found");
    const int top = lua_gettop(state);
    lua_pushinteger(state, index - 1);
    LuaBridge_InvokeMethod(state, getter, object, top + 1);
    // Internal callers (each/dump) have no Lua C-function return boundary to discard arguments.
    lua_remove(state, top + 1);
    return 1;
}

int LuaBridge_ContainerNewIndex(lua_State* state, MonoObject* object, lua_Integer index, int valueIndex)
{
    auto& ownerResolver = MonoResolver::Instance();
    ScopedGCHandle owner(ownerResolver, ownerResolver.CreateGCHandle(object, true));
    if (!owner.value) return LuaEngine::RaiseBridgeError(state, "failed to pin container");
    object = owner.Target();
    const int length = LuaBridge_ContainerLength(state, object);
    if (index < 1 || index > length)
    {
        char message[96];
        sprintf_s(message, "container index out of range: %lld", static_cast<long long>(index));
        return LuaEngine::RaiseBridgeError(state, "%s", message);
    }
    if (LuaBridge_IsArray(object))
    {
        auto& resolver = MonoResolver::Instance();
        MonoClass* arrayClass = resolver.ObjectClass(object);
        MonoType* elementType = resolver.ClassType(resolver.ElementClass(arrayClass));
        std::string error;
        if (!LuaBridge_WriteArrayValue(state, valueIndex, reinterpret_cast<MonoArray*>(object), elementType,
                                       static_cast<uintptr_t>(index - 1), error))
            return LuaEngine::RaiseBridgeError(state, "%s", error.c_str());
        return 0;
    }
    MonoMethod* setter = FindInstanceMethod(MonoResolver::Instance().ObjectClass(object), "set_Item", 2);
    if (!setter) return LuaEngine::RaiseBridgeError(state, "List<T>.set_Item was not found");
    valueIndex = lua_absindex(state, valueIndex);
    const int top = lua_gettop(state);
    lua_pushinteger(state, index - 1);
    lua_pushvalue(state, valueIndex);
    LuaBridge_InvokeMethod(state, setter, object, top + 1);
    lua_settop(state, top);
    return 0;
}
