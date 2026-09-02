/**
 * ============================================================
 * lua_container.cpp — Mono 数组与 List<T> 容器桥接
 * ============================================================
 * 数组通过 Mono API 操作；List<T> 通过 get_Count/get_Item/set_Item 反射
 * 调用，不把 _items/_size 私有布局作为主实现。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include "mono_resolver.h"

extern "C" {
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
                    (resolver.MethodFlags(method) & 0x0010) == 0 &&
                    resolver.MethodParameters(method).size() == parameterCount)
                    return method;
            }
        }
        return nullptr;
    }
}

bool LuaBridge_IsArray(MonoObject* object)
{
    if (!object) return false;
    auto& resolver = MonoResolver::Instance();
    return resolver.ClassRank(resolver.ObjectClass(object)) > 0;
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
        if (resolver.ClassRank(resolver.ObjectClass(object)) != 1)
            luaL_error(state, "multidimensional arrays are not supported");
        const uintptr_t length = resolver.ArrayLength(reinterpret_cast<MonoArray*>(object));
        if (length > static_cast<uintptr_t>(INT32_MAX))
            luaL_error(state, "array length exceeds the Lua container limit");
        return static_cast<int>(length);
    }
    if (!LuaBridge_IsList(object)) luaL_error(state, "Instance is not an array or List<T>");
    auto& resolver = MonoResolver::Instance();
    MonoMethod* getter = FindInstanceMethod(resolver.ObjectClass(object), "get_Count", 0);
    if (!getter) luaL_error(state, "List<T>.get_Count was not found");
    MonoObject* exception = nullptr;
    MonoObject* result = resolver.Invoke(getter, object, nullptr, &exception);
    if (exception) luaL_error(state, "managed exception: %s", resolver.ObjectString(exception).c_str());
    void* value = resolver.Unbox(result);
    if (!value) luaL_error(state, "List<T>.Count returned no value");
    return *static_cast<int32_t*>(value);
}

int LuaBridge_ContainerIndex(lua_State* state, MonoObject* object, lua_Integer index)
{
    const int length = LuaBridge_ContainerLength(state, object);
    if (index < 1 || index > length) return luaL_error(state, "container index out of range: %lld", static_cast<long long>(index));
    if (LuaBridge_IsArray(object))
    {
        auto& resolver = MonoResolver::Instance();
        MonoClass* arrayClass = resolver.ObjectClass(object);
        MonoClass* elementClass = resolver.ElementClass(arrayClass);
        MonoType* elementType = resolver.ClassType(elementClass);
        const int elementSize = resolver.ArrayElementSize(arrayClass);
        void* address = resolver.ArrayAddress(
            reinterpret_cast<MonoArray*>(object), elementSize, static_cast<uintptr_t>(index - 1));
        std::string error;
        if (!LuaBridge_PushRawValue(state, elementType, address, error))
            return luaL_error(state, "%s", error.c_str());
        return 1;
    }
    MonoMethod* getter = FindInstanceMethod(MonoResolver::Instance().ObjectClass(object), "get_Item", 1);
    if (!getter) return luaL_error(state, "List<T>.get_Item was not found");
    lua_pushinteger(state, index - 1);
    const int result = LuaBridge_InvokeMethod(state, getter, object, lua_gettop(state));
    lua_remove(state, -result - 1);
    return result;
}

int LuaBridge_ContainerNewIndex(
    lua_State* state, MonoObject* object, lua_Integer index, int valueIndex)
{
    const int length = LuaBridge_ContainerLength(state, object);
    if (index < 1 || index > length) return luaL_error(state, "container index out of range: %lld", static_cast<long long>(index));
    if (LuaBridge_IsArray(object))
    {
        auto& resolver = MonoResolver::Instance();
        MonoClass* arrayClass = resolver.ObjectClass(object);
        MonoType* elementType = resolver.ClassType(resolver.ElementClass(arrayClass));
        std::string error;
        if (!LuaBridge_WriteArrayValue(state, valueIndex,
            reinterpret_cast<MonoArray*>(object), elementType,
            static_cast<uintptr_t>(index - 1), error))
            return luaL_error(state, "%s", error.c_str());
        return 0;
    }
    MonoMethod* setter = FindInstanceMethod(MonoResolver::Instance().ObjectClass(object), "set_Item", 2);
    if (!setter) return luaL_error(state, "List<T>.set_Item was not found");
    lua_pushinteger(state, index - 1);
    lua_pushvalue(state, valueIndex);
    return LuaBridge_InvokeMethod(state, setter, object, lua_gettop(state) - 1);
}
