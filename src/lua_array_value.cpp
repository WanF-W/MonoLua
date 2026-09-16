/**
 * lua_value.cpp — Lua ↔ Mono 值转换中心
 * 本模块集中实现字段、数组、方法参数和返回值的 Lua/Mono 转换，
 * Class/Method/Field/Instance Binding 不复制编组代码。跨 Mono 分配保存的
 * 对象使用短期 pinned GCHandle；不完整的 ref/out 与 Nullable 语义明确拒绝。
 */
#include "lua_binding_internal.h"
#include "mono_metadata.h"
#include "mono_resolver.h"
#include "lua_value_internal.h"
#include "mono_handle.h"
#include "mono_feature_fault.h"

using namespace mono_value;
using mono::ScopedGCHandle;

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

#include "lua_value_memory.h"
using namespace mono_value_memory;
using namespace mono_metadata;

bool LuaBridge_WriteArrayValue(lua_State* state, int valueIndex, MonoArray* array, MonoType* type,
                               uintptr_t index, std::string& error)
{
    if (!array || !type)
    {
        error = "array or element type is null";
        return false;
    }
    auto& resolver = MonoResolver::Instance();
    // ArrayAddress 返回的是数组内部地址。即使调用方持有普通强 handle，
    // 中间的 Mono API 也可能触发移动式 GC；这里固定数组直到本次写入结束。
    ScopedGCHandle arrayHandle(resolver, resolver.CreateGCHandle(reinterpret_cast<MonoObject*>(array), true));
    if (!arrayHandle.value)
    {
        error = "failed to pin the Mono array";
        return false;
    }
    array = reinterpret_cast<MonoArray*>(arrayHandle.Target());
    if (!array)
    {
        error = "Mono array is no longer available";
        return false;
    }
    MonoClass* arrayClass = resolver.ObjectClass(reinterpret_cast<MonoObject*>(array));
    const int elementSize = resolver.ArrayElementSize(arrayClass);
    if (elementSize <= 0)
    {
        error = "array element size is invalid";
        return false;
    }
    void* address = resolver.ArrayAddress(array, elementSize, index);
    if (!address)
    {
        error = "array element address is null";
        return false;
    }
    if (IsNullable(resolver, type))
    {
        error = "Nullable values are not supported";
        return false;
    }
    const int declaredKind = resolver.TypeKind(type);
    const int kind = StorageTypeKind(resolver, type);
    if ((kind >= TYPE_BOOLEAN && kind <= TYPE_R8) || kind == TYPE_I || kind == TYPE_U)
    {
        uint64_t storage = 0;
        if (elementSize > static_cast<int>(sizeof(storage)))
        {
            error = "invalid scalar array element size";
            return false;
        }
        if (!LuaBridge_ReadScalarValue(state, valueIndex, kind, &storage, error)) return false;
        if (!WriteValueSafely(address, &storage, static_cast<size_t>(elementSize)))
        {
            error = "failed to write array element";
            return false;
        }
        return true;
    }
    if (kind == TYPE_VALUETYPE)
    {
        if (!lua_isnil(state, valueIndex) && !luaL_testudata(state, valueIndex, LuaBridgeMT::INSTANCE))
        {
            error = "value-type array element requires a boxed Instance";
            return false;
        }
        MonoObject* boxed = nullptr;
        MonoClass* expected = resolver.TypeClass(type);
        if (lua_isnil(state, valueIndex))
            boxed = resolver.NewObject(resolver.ObjectDomain(reinterpret_cast<MonoObject*>(array)), expected);
        else if (!LuaBridge_TryGetInstanceObject(state, valueIndex, boxed))
        {
            error = "boxed value-type Instance is no longer available";
            return false;
        }
        if (!expected || !boxed || resolver.ObjectClass(boxed) != expected)
        {
            error = "boxed Instance type is incompatible with array element type";
            return false;
        }
        ScopedGCHandle boxedRoot(resolver, resolver.CreateGCHandle(boxed, true));
        if (!boxedRoot.value)
        {
            error = "failed to pin boxed value";
            return false;
        }
        void* unboxed = resolver.Unbox(boxedRoot.Target());
        if (!unboxed)
        {
            error = "failed to unbox array element";
            return false;
        }
        if (!resolver.CopyValue(address, unboxed, expected))
        {
            error = "this Mono runtime cannot copy value-type array elements";
            return false;
        }
        return true;
    }
    if (!IsReferenceType(resolver, type, declaredKind))
    {
        error = "unsupported Mono array element type";
        return false;
    }
    MonoObject* value = nullptr;
    if (!lua_isnil(state, valueIndex))
    {
        if (kind == TYPE_STRING)
        {
            if (lua_type(state, valueIndex) != LUA_TSTRING)
            {
                error = "array value must be a string or nil";
                return false;
            }
            value = reinterpret_cast<MonoObject*>(
                resolver.NewString(resolver.ObjectDomain(reinterpret_cast<MonoObject*>(array)),
                                   lua_tostring(state, valueIndex), lua_rawlen(state, valueIndex)));
            if (!value)
            {
                error = "failed to create the Mono string array value";
                return false;
            }
        }
        else
        {
            if (!LuaBridge_TryGetInstanceObject(state, valueIndex, value))
            {
                error = "array value must be a valid Instance or nil";
                return false;
            }
            MonoClass* expected = resolver.TypeClass(type);
            if (expected && !resolver.ObjectIsInstanceOf(value, expected))
            {
                error = "Instance type is incompatible with array element type";
                return false;
            }
        }
    }
    ScopedGCHandle valueHandle(resolver, value ? resolver.CreateGCHandle(value, true) : 0);
    if (value && !valueHandle.value)
    {
        error = "failed to pin the array element";
        return false;
    }
    // 整个操作期间固定数组；所有引用写入都使用 Mono 写屏障。
    if (!resolver.SetArrayReference(array, address, valueHandle.Target()))
    {
        error = "this Mono runtime cannot write reference array elements";
        return false;
    }
    return true;
}
