// Method overload selection, argument ownership and runtime_invoke.
#include "lua_binding_internal.h"
#include "lua_value_internal.h"
#include "lua_engine.h"
#include "mono_handle.h"
extern "C"
{
#include "lauxlib.h"
}
using namespace mono_metadata;
using namespace mono_value;
using mono::ScopedGCHandle;

namespace
{
    int ScoreArgument(lua_State* state, int index, MonoType* type)
    {
        auto& resolver = MonoResolver::Instance();
        if (!type || resolver.TypeIsByRef(type) || IsNullable(resolver, type)) return INT32_MIN;
        const int kind = StorageTypeKind(resolver, type);
        if (kind == TYPE_BOOLEAN) return lua_isboolean(state, index) ? 30 : INT32_MIN;
        if ((kind >= TYPE_CHAR && kind <= TYPE_U8) || kind == TYPE_I || kind == TYPE_U)
        {
            if ((kind == TYPE_I || kind == TYPE_U) && lua_islightuserdata(state, index)) return 30;
            int exact = 0;
            const lua_Integer value = lua_tointegerx(state, index, &exact);
            if (lua_type(state, index) != LUA_TNUMBER || !exact || !IntegerFits(kind, value))
                return INT32_MIN;
            if (!lua_isinteger(state, index)) return 10;
            return kind == TYPE_I4 || kind == TYPE_U4 ? 30 : 20;
        }
        if (kind == TYPE_R4 || kind == TYPE_R8)
            return lua_type(state, index) == LUA_TNUMBER ? (lua_isinteger(state, index) ? 10 : 30)
                                                         : INT32_MIN;
        if (kind == TYPE_STRING)
            return lua_isnil(state, index) ? 20 : (lua_type(state, index) == LUA_TSTRING ? 30 : INT32_MIN);
        const bool reference = IsReferenceType(resolver, type, resolver.TypeKind(type));
        if (reference && lua_isnil(state, index)) return 20;
        if (kind == TYPE_VALUETYPE && lua_isnil(state, index)) return 20;
        if (!reference && kind != TYPE_VALUETYPE) return INT32_MIN;
        MonoObject* object = nullptr;
        if (!LuaBridge_TryGetInstanceObject(state, index, object)) return INT32_MIN;
        MonoClass* expected = resolver.TypeClass(type);
        if (!expected) return INT32_MIN;
        if (resolver.ObjectClass(object) == expected) return 30;
        return reference && resolver.ObjectIsInstanceOf(object, expected) ? 10 : INT32_MIN;
    }

    int ScoreArguments(lua_State* state, int first, const std::vector<MonoType*>& types)
    {
        if (lua_gettop(state) - first + 1 != static_cast<int>(types.size())) return INT32_MIN;
        int score = 0;
        for (size_t index = 0; index < types.size(); ++index)
        {
            const int current = ScoreArgument(state, first + static_cast<int>(index), types[index]);
            if (current == INT32_MIN) return current;
            score += current;
        }
        return score;
    }
} // namespace

MonoMethod* LuaBridge_FindBestMethod(lua_State* state, MonoClass* klass, const char* name, int firstArgument,
                                     bool staticOnly)
{
    if (!klass || !name) return nullptr;
    auto& resolver = MonoResolver::Instance();
    const bool constructor = strcmp(name, ".ctor") == 0;
    for (MonoClass* current = klass; current; current = resolver.ClassParent(current))
    {
        MonoMethod* matched = nullptr;
        int bestScore = INT32_MIN;
        for (MonoMethod* method : resolver.EnumerateMethods(current))
        {
            const char* methodName = resolver.MethodName(method);
            if (!methodName || strcmp(methodName, name) != 0) continue;
            if (staticOnly && !(resolver.MethodFlags(method) & METHOD_ATTRIBUTE_STATIC)) continue;
            const int score = ScoreArguments(state, firstArgument, resolver.MethodParameters(method));
            if (score > bestScore)
            {
                bestScore = score;
                matched = method;
            }
        }
        // Derived declarations first; equal scores retain enumeration order, as in Il2CppLua.
        if (matched || constructor) return matched;
    }
    return nullptr;
}

int LuaBridge_InvokeMethod(lua_State* state, MonoMethod* method, MonoObject* object, int firstArgument,
                           LuaMethodInvocation* invocation)
{
    auto& resolver = MonoResolver::Instance();
    if (!method) return LuaEngine::RaiseBridgeError(state, "method is null");
    if (resolver.MethodFlags(method) & METHOD_ATTRIBUTE_STATIC) object = nullptr;
    ScopedGCHandle owner(resolver, object ? resolver.CreateGCHandle(object, true) : 0);
    if (object && !owner.value) return LuaEngine::RaiseBridgeError(state, "failed to pin the method target");
    const uint32_t objectHandle = owner.value;
    // Hook preparation already rejects every generic signature. Ordinary calls must
    // distinguish open definitions from constructed methods before runtime_invoke.
    if (!invocation)
    {
        bool open = false;
        if (!MonoRuntime::Instance().InspectOpenMethod(method, open))
            return LuaEngine::RaiseBridgeError(state, "cannot verify whether the method contains generic parameters");
        if (open) return LuaEngine::RaiseBridgeError(state, "open generic methods or declaring types cannot be invoked");
    }
    const auto types = resolver.MethodParameters(method);
    MonoType* returnType = resolver.MethodReturnType(method);
    if (!returnType || resolver.TypeIsByRef(returnType) || IsNullable(resolver, returnType))
        return LuaEngine::RaiseBridgeError(state, "ref and Nullable returns are not supported");
    const bool captured = invocation && invocation->arguments && invocation->handles;
    if (captured && (invocation->arguments->size() != types.size() || invocation->handles->size() != types.size()))
        return LuaEngine::RaiseBridgeError(state, "captured argument count is incompatible with the method");
    if (!captured && ScoreArguments(state, firstArgument, types) == INT32_MIN)
        return LuaEngine::RaiseBridgeError(state, "incompatible method arguments (ref/out and Nullable are unsupported)");

    // Lua 以 C++ 编译，参数根在普通错误路径上正常析构。
    std::vector<uint64_t> storage(types.size(), 0);
    std::vector<void*> parameters(types.size(), nullptr);
    mono::GCHandles roots(types.size());
    auto& parameterHandles = roots.values;
    if (objectHandle) object = resolver.GCHandleTarget(objectHandle);
    MonoDomain* objectDomain = object ? resolver.ObjectDomain(object) : MonoRuntime::Instance().Domain();
    for (size_t index = 0; index < types.size(); ++index)
    {
        const int luaIndex = firstArgument + static_cast<int>(index);
        MonoType* type = types[index];
        const int kind = StorageTypeKind(resolver, type);
        void* slot = &storage[index];

        if (captured)
        {
            storage[index] = (*invocation->arguments)[index];
            parameters[index] = IsReferenceType(resolver, type, resolver.TypeKind(type))
                ? resolver.GCHandleTarget((*invocation->handles)[index]) : slot;
            continue;
        }

        if ((kind >= TYPE_BOOLEAN && kind <= TYPE_R8) || kind == TYPE_I || kind == TYPE_U)
        {
            std::string error;
            if (!LuaBridge_ReadScalarValue(state, luaIndex, kind, slot, error))
                return LuaEngine::RaiseBridgeError(state, "%s", error.c_str());
        }
        else
        {
            MonoObject* value = nullptr;
            if (kind == TYPE_VALUETYPE && lua_isnil(state, luaIndex))
            {
                value = resolver.NewObject(objectDomain, resolver.TypeClass(type));
                if (!value) return LuaEngine::RaiseBridgeError(state, "failed to allocate default value-type argument");
            }
            else if (kind == TYPE_STRING && !lua_isnil(state, luaIndex))
            {
                value = reinterpret_cast<MonoObject*>(resolver.NewString(
                    objectDomain, lua_tostring(state, luaIndex), lua_rawlen(state, luaIndex)));
                if (!value) return LuaEngine::RaiseBridgeError(state, "failed to create string argument");
            }
            else if (!lua_isnil(state, luaIndex))
                value = LuaBridge_GetInstanceObject(state, luaIndex);
            if (value)
            {
                parameterHandles[index] = resolver.CreateGCHandle(value, true);
                if (!parameterHandles[index]) return LuaEngine::RaiseBridgeError(state, "failed to pin argument");
            }
            continue;
        }
        parameters[index] = slot;
    }

    // 参数编组可能分配多个托管字符串。调用前从 pinned handle 重新解析
    // 所有引用，确保传给 mono_runtime_invoke 的地址属于当前 GC 周期。
    for (size_t index = 0; index < types.size(); ++index)
    {
        if (!parameterHandles[index]) continue;
        MonoType* type = types[index];
        const int kind = StorageTypeKind(resolver, type);
        MonoObject* target = resolver.GCHandleTarget(parameterHandles[index]);
        parameters[index] = kind == TYPE_VALUETYPE ? resolver.Unbox(target) : target;
    }
    if (objectHandle) object = resolver.GCHandleTarget(objectHandle);

    MonoObject* exception = nullptr;
    void* target = object;
    if (object && resolver.ClassIsValueType(resolver.MethodClass(method))) target = resolver.Unbox(object);
    if (invocation && invocation->beforeInvoke) invocation->beforeInvoke(invocation->context);
    if (invocation && invocation->entered) *invocation->entered = true;
    MonoObject* result =
        resolver.Invoke(method, target, parameters.empty() ? nullptr : parameters.data(), &exception);
    LuaEngine::Instance().CheckHealthy();
    // mono_runtime_invoke 返回的对象是借用指针。释放参数根或继续调用其他
    // Mono API 都可能触发 GC，因此先把结果固定到本地作用域结束，直到它
    // 被复制成 Lua 字符串或新的 Instance GCHandle。
    ScopedGCHandle resultHandle(resolver, result ? resolver.CreateGCHandle(result, true) : 0);
    ScopedGCHandle exceptionHandle(resolver, exception ? resolver.CreateGCHandle(exception, true) : 0);
    if (result && !resultHandle.value)
    {
        return LuaEngine::RaiseBridgeError(state, "failed to retain method result");
    }
    if (exception && !exceptionHandle.value)
    {
        return LuaEngine::RaiseBridgeError(state, "failed to retain managed exception");
    }
    if (resultHandle.value) result = resultHandle.Target();
    if (exceptionHandle.value) exception = exceptionHandle.Target();
    if (exception)
    {
        const std::string message = resolver.ObjectString(exception);
        return LuaEngine::RaiseManagedError(state, message.empty() ? "<unknown>" : message.c_str());
    }

    if (invocation && invocation->resultHandle)
    {
        // Preserve the completed native result before converting it to a Lua value.
        // A later Lua allocation/conversion error must not discard this result or retry.
        if (invocation->resultInt && invocation->resultFloat)
        {
            const int kind = StorageTypeKind(resolver, returnType);
            if (IsReferenceType(resolver, returnType, resolver.TypeKind(returnType)))
                *invocation->resultInt = reinterpret_cast<uint64_t>(result);
            else if (result && kind != TYPE_VOID)
            {
                size_t bytes = 8;
                if (kind == TYPE_BOOLEAN || kind == TYPE_I1 || kind == TYPE_U1) bytes = 1;
                else if (kind == TYPE_CHAR || kind == TYPE_I2 || kind == TYPE_U2) bytes = 2;
                else if (kind == TYPE_I4 || kind == TYPE_U4 || kind == TYPE_R4) bytes = 4;
                uint64_t raw = 0;
                memcpy(&raw, resolver.Unbox(result), bytes);
                *(kind == TYPE_R4 || kind == TYPE_R8 ? invocation->resultFloat : invocation->resultInt) = raw;
            }
        }
        resolver.FreeGCHandle(*invocation->resultHandle);
        *invocation->resultHandle = resultHandle.value;
        resultHandle.value = 0; // retained by the active Hook until its return path completes
    }

    const int declaredKind = resolver.TypeKind(returnType);
    const int kind = StorageTypeKind(resolver, returnType);
    if (kind == TYPE_VOID) return 0;
    if (!result)
    {
        lua_pushnil(state);
        return 1;
    }
    if (kind == TYPE_VALUETYPE)
    {
        LuaBridge_PushInstance(state, result);
        return 1;
    }
    void* address = IsReferenceType(resolver, returnType, declaredKind) ? static_cast<void*>(&result)
                                                                        : resolver.Unbox(result);
    std::string error;
    if (!LuaBridge_PushRawValue(state, returnType, address, error))
        return LuaEngine::RaiseBridgeError(state, "%s", error.c_str());
    return 1;
}
