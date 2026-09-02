/**
 * ============================================================
 * lua_value.cpp — Lua ↔ Mono 值转换中心
 * ============================================================
 * 本模块集中实现字段、数组、方法参数和返回值的 Lua/Mono 转换，
 * Class/Method/Field/Instance Binding 不复制编组代码。跨 Mono 分配保存的
 * 对象使用短期 pinned GCHandle；不完整的 ref/out 与 Nullable 语义明确拒绝。
 * ============================================================
 */
#include "lua_binding_internal.h"
#include "mono_resolver.h"

#include <limits>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    // 数值与 MonoTypeEnum 保持一致；项目动态解析 Mono，不引入某个 Unity
    // 版本附带的 Mono 头文件，避免编译期结构声明反过来绑定运行时版本。
    enum MonoTypeKind
    {
        TYPE_VOID = 0x01, TYPE_BOOLEAN = 0x02, TYPE_CHAR = 0x03,
        TYPE_I1 = 0x04, TYPE_U1 = 0x05, TYPE_I2 = 0x06, TYPE_U2 = 0x07,
        TYPE_I4 = 0x08, TYPE_U4 = 0x09, TYPE_I8 = 0x0A, TYPE_U8 = 0x0B,
        TYPE_R4 = 0x0C, TYPE_R8 = 0x0D, TYPE_STRING = 0x0E,
        TYPE_BYREF = 0x10,
        TYPE_VALUETYPE = 0x11, TYPE_CLASS = 0x12, TYPE_ARRAY = 0x14,
        TYPE_GENERICINST = 0x15, TYPE_I = 0x18, TYPE_U = 0x19,
        TYPE_OBJECT = 0x1C, TYPE_SZARRAY = 0x1D
    };

    union FieldStorage
    {
        int8_t i1; uint8_t u1;
        int16_t i2; uint16_t u2;
        int32_t i4; uint32_t u4;
        int64_t i8; uint64_t u8;
        float r4; double r8;
        intptr_t nativeInt; uintptr_t nativeUInt;
        MonoObject* object;
    };

    bool IsReferenceType(MonoResolver& resolver, MonoType* type, int kind)
    {
        if (kind == TYPE_STRING || kind == TYPE_CLASS || kind == TYPE_OBJECT ||
            kind == TYPE_ARRAY || kind == TYPE_SZARRAY)
            return true;
        if (kind != TYPE_GENERICINST) return false;
        MonoClass* klass = resolver.TypeClass(type);
        return klass && !resolver.ClassIsValueType(klass);
    }

    int StorageTypeKind(MonoResolver& resolver, MonoType* type)
    {
        const int kind = resolver.TypeKind(type);
        if (kind == TYPE_GENERICINST)
        {
            MonoClass* genericClass = resolver.TypeClass(type);
            return genericClass && resolver.ClassIsValueType(genericClass)
                ? TYPE_VALUETYPE : kind;
        }
        if (kind != TYPE_VALUETYPE) return kind;
        MonoClass* klass = resolver.TypeClass(type);
        if (!klass || !resolver.ClassIsEnum(klass)) return kind;
        return resolver.TypeKind(resolver.EnumBaseType(klass));
    }

    bool RequireInteger(lua_State* state, int index, lua_Integer& value, std::string& error)
    {
        if (!lua_isinteger(state, index))
        {
            error = "field value must be an integer";
            return false;
        }
        value = lua_tointeger(state, index);
        return true;
    }

    bool IntegerFits(int kind, lua_Integer value)
    {
        switch (kind)
        {
        case TYPE_CHAR: case TYPE_U2:
            return value >= 0 && value <= (std::numeric_limits<uint16_t>::max)();
        case TYPE_I1:
            return value >= (std::numeric_limits<int8_t>::min)() &&
                value <= (std::numeric_limits<int8_t>::max)();
        case TYPE_U1:
            return value >= 0 && value <= (std::numeric_limits<uint8_t>::max)();
        case TYPE_I2:
            return value >= (std::numeric_limits<int16_t>::min)() &&
                value <= (std::numeric_limits<int16_t>::max)();
        case TYPE_I4:
            return value >= (std::numeric_limits<int32_t>::min)() &&
                value <= (std::numeric_limits<int32_t>::max)();
        case TYPE_U4:
            return value >= 0 && static_cast<uint64_t>(value) <=
                (std::numeric_limits<uint32_t>::max)();
        case TYPE_U8: case TYPE_U:
            return value >= 0;
        default:
            return true;
        }
    }

    bool RequireIntegerForKind(
        lua_State* state, int index, int kind, lua_Integer& value,
        std::string& error)
    {
        if (!RequireInteger(state, index, value, error)) return false;
        if (IntegerFits(kind, value)) return true;
        error = "integer value is outside the target Mono type range";
        return false;
    }

    bool IsNullable(MonoResolver& resolver, MonoType* type)
    {
        if (resolver.TypeKind(type) != TYPE_GENERICINST) return false;
        MonoClass* klass = resolver.TypeClass(type);
        const char* name = resolver.ClassName(klass);
        const char* nameSpace = resolver.ClassNamespace(klass);
        return name && nameSpace && strcmp(name, "Nullable`1") == 0 &&
            strcmp(nameSpace, "System") == 0;
    }

    // Mono 的 byref MonoType 仍可通过 mono_class_from_mono_type 取得目标类。
    // 再取该类的规范类型即可复用普通参数编组；调用时是否需要二级槽位，
    // 由原始参数上的 TYPE_BYREF 标记单独决定，不能在这里丢失。
    MonoType* EffectiveParameterType(MonoResolver& resolver, MonoType* type)
    {
        if (!type || !resolver.TypeIsByRef(type)) return type;
        MonoClass* targetClass = resolver.TypeClass(type);
        return targetClass ? resolver.ClassType(targetClass) : nullptr;
    }
}

bool LuaBridge_CanMarshalMethodArguments(
    lua_State* state, int firstArgument, const std::vector<MonoType*>& types)
{
    if (lua_gettop(state) - firstArgument + 1 != static_cast<int>(types.size())) return false;
    auto& resolver = MonoResolver::Instance();
    for (size_t index = 0; index < types.size(); ++index)
    {
        // ref/out 需要把调用后的值重新返回 Lua。当前公共调用约定没有定义
        // 多返回值布局，因此在实现完整语义前明确拒绝，避免静默丢失结果。
        if (resolver.TypeIsByRef(types[index])) return false;
        const int luaIndex = firstArgument + static_cast<int>(index);
        MonoType* effectiveType = EffectiveParameterType(resolver, types[index]);
        if (!effectiveType) return false;
        if (IsNullable(resolver, effectiveType)) return false;
        const int declaredKind = resolver.TypeKind(effectiveType);
        const int kind = StorageTypeKind(resolver, effectiveType);
        if (kind == TYPE_BOOLEAN && !lua_isboolean(state, luaIndex)) return false;
        if ((kind >= TYPE_CHAR && kind <= TYPE_U8) || kind == TYPE_I || kind == TYPE_U)
        {
            if (!lua_isinteger(state, luaIndex) ||
                !IntegerFits(kind, lua_tointeger(state, luaIndex))) return false;
        }
        else if ((kind == TYPE_R4 || kind == TYPE_R8) && !lua_isnumber(state, luaIndex)) return false;
        else if (kind == TYPE_STRING && !lua_isnil(state, luaIndex) && lua_type(state, luaIndex) != LUA_TSTRING) return false;
        else if (IsReferenceType(resolver, effectiveType, declaredKind) &&
            !lua_isnil(state, luaIndex) && !luaL_testudata(state, luaIndex, LuaBridgeMT::INSTANCE)) return false;
        else if (kind == TYPE_VALUETYPE && !luaL_testudata(state, luaIndex, LuaBridgeMT::INSTANCE)) return false;
        const bool scalar = (kind >= TYPE_BOOLEAN && kind <= TYPE_STRING) ||
            kind == TYPE_I || kind == TYPE_U;
        const bool boxedValue = declaredKind == TYPE_VALUETYPE && kind == TYPE_VALUETYPE &&
            luaL_testudata(state, luaIndex, LuaBridgeMT::INSTANCE) != nullptr;
        if (!scalar && !boxedValue && !IsReferenceType(resolver, effectiveType, declaredKind)) return false;
    }
    return true;
}

int LuaBridge_ScoreMethodArguments(
    lua_State* state, int firstArgument, const std::vector<MonoType*>& types)
{
    if (!LuaBridge_CanMarshalMethodArguments(state, firstArgument, types)) return INT32_MIN;
    auto& resolver = MonoResolver::Instance();
    int score = 0;
    for (size_t index = 0; index < types.size(); ++index)
    {
        MonoType* type = EffectiveParameterType(resolver, types[index]);
        const int declaredKind = resolver.TypeKind(type);
        const int kind = StorageTypeKind(resolver, type);
        const int luaIndex = firstArgument + static_cast<int>(index);
        if (lua_isboolean(state, luaIndex)) score += kind == TYPE_BOOLEAN ? 30 : 0;
        else if (lua_isinteger(state, luaIndex))
        {
            if (kind == TYPE_I4 || kind == TYPE_U4) score += 30;
            else if ((kind >= TYPE_CHAR && kind <= TYPE_U8) || kind == TYPE_I || kind == TYPE_U) score += 20;
            else score += 10;
        }
        else if (lua_isnumber(state, luaIndex)) score += (kind == TYPE_R4 || kind == TYPE_R8) ? 30 : 10;
        else if (lua_type(state, luaIndex) == LUA_TSTRING) score += kind == TYPE_STRING ? 30 : 0;
        else if (lua_isnil(state, luaIndex)) score += IsReferenceType(resolver, type, declaredKind) ? 20 : 0;
        else if (luaL_testudata(state, luaIndex, LuaBridgeMT::INSTANCE))
        {
            MonoObject* object = LuaBridge_GetInstanceObject(state, luaIndex);
            MonoClass* expected = resolver.TypeClass(type);
            if (expected && !resolver.ObjectIsInstanceOf(object, expected) &&
                resolver.ObjectClass(object) != expected)
                return INT32_MIN;
            score += expected && resolver.ObjectClass(object) == expected ? 40 : 30;
        }
        if (!resolver.TypeIsByRef(types[index])) ++score;
    }
    return score;
}

int LuaBridge_InvokeMethod(
    lua_State* state, MonoMethod* method, MonoObject* object, int firstArgument)
{
    auto& resolver = MonoResolver::Instance();
    const auto types = resolver.MethodParameters(method);
    if (lua_gettop(state) - firstArgument + 1 != static_cast<int>(types.size()))
        return luaL_error(state, "method argument count mismatch");
    for (MonoType* type : types)
    {
        if (resolver.TypeIsByRef(type))
            return luaL_error(state, "ref/out parameters are not supported");
    }
    if (!LuaBridge_CanMarshalMethodArguments(state, firstArgument, types))
        return luaL_error(state, "one or more method arguments are incompatible");

    // 类型兼容性必须在创建任何临时 handle 前完成。luaL_error 使用 longjmp，
    // 若在错误路径跨过 C++ 析构或手工 handle 清理会造成资源泄漏。
    for (size_t index = 0; index < types.size(); ++index)
    {
        MonoType* type = EffectiveParameterType(resolver, types[index]);
        const int declaredKind = resolver.TypeKind(type);
        const int kind = StorageTypeKind(resolver, type);
        const int luaIndex = firstArgument + static_cast<int>(index);
        if (!luaL_testudata(state, luaIndex, LuaBridgeMT::INSTANCE)) continue;
        MonoObject* value = LuaBridge_GetInstanceObject(state, luaIndex);
        MonoClass* expected = resolver.TypeClass(type);
        const bool compatible = declaredKind == TYPE_VALUETYPE && kind == TYPE_VALUETYPE
            ? expected && resolver.ObjectClass(value) == expected
            : !expected || resolver.ObjectIsInstanceOf(value, expected);
        if (!compatible)
            return luaL_error(state, "argument %d has an incompatible Instance type",
                static_cast<int>(index + 1));
    }

    std::vector<uint64_t> storage(types.size(), 0);
    std::vector<void*> parameters(types.size(), nullptr);
    std::vector<uint32_t> parameterHandles(types.size(), 0);
    const uint32_t objectHandle = object
        ? resolver.CreateGCHandle(object, true) : 0;
    if (object && !objectHandle)
        return luaL_error(state, "failed to pin the method target");
    const auto freeTemporaryHandles = [&]()
    {
        for (uint32_t handle : parameterHandles)
            if (handle) resolver.FreeGCHandle(handle);
        if (objectHandle) resolver.FreeGCHandle(objectHandle);
    };
    if (objectHandle) object = resolver.GCHandleTarget(objectHandle);
    MonoDomain* objectDomain = object
        ? resolver.ObjectDomain(object) : MonoRuntime::Instance().Domain();
    for (size_t index = 0; index < types.size(); ++index)
    {
        const int luaIndex = firstArgument + static_cast<int>(index);
        MonoType* originalType = types[index];
        MonoType* type = EffectiveParameterType(resolver, originalType);
        if (!type)
        {
            freeTemporaryHandles();
            return luaL_error(state, "failed to resolve argument %d type",
                static_cast<int>(index + 1));
        }
        const int declaredKind = resolver.TypeKind(type);
        const int kind = StorageTypeKind(resolver, type);
        void* slot = &storage[index];

        if (kind == TYPE_BOOLEAN) *static_cast<uint8_t*>(slot) = static_cast<uint8_t>(lua_toboolean(state, luaIndex));
        else if (kind == TYPE_CHAR || kind == TYPE_U2) *static_cast<uint16_t*>(slot) = static_cast<uint16_t>(luaL_checkinteger(state, luaIndex));
        else if (kind == TYPE_I1) *static_cast<int8_t*>(slot) = static_cast<int8_t>(luaL_checkinteger(state, luaIndex));
        else if (kind == TYPE_U1) *static_cast<uint8_t*>(slot) = static_cast<uint8_t>(luaL_checkinteger(state, luaIndex));
        else if (kind == TYPE_I2) *static_cast<int16_t*>(slot) = static_cast<int16_t>(luaL_checkinteger(state, luaIndex));
        else if (kind == TYPE_I4) *static_cast<int32_t*>(slot) = static_cast<int32_t>(luaL_checkinteger(state, luaIndex));
        else if (kind == TYPE_U4) *static_cast<uint32_t*>(slot) = static_cast<uint32_t>(luaL_checkinteger(state, luaIndex));
        else if (kind == TYPE_I8 || kind == TYPE_I) *static_cast<int64_t*>(slot) = static_cast<int64_t>(luaL_checkinteger(state, luaIndex));
        else if (kind == TYPE_U8 || kind == TYPE_U) *static_cast<uint64_t*>(slot) = static_cast<uint64_t>(luaL_checkinteger(state, luaIndex));
        else if (kind == TYPE_R4) *static_cast<float*>(slot) = static_cast<float>(luaL_checknumber(state, luaIndex));
        else if (kind == TYPE_R8) *static_cast<double*>(slot) = static_cast<double>(luaL_checknumber(state, luaIndex));
        else if (kind == TYPE_STRING)
        {
            MonoObject* value = reinterpret_cast<MonoObject*>(
                lua_isnil(state, luaIndex) ? nullptr : resolver.NewString(
                    objectDomain, luaL_checkstring(state, luaIndex)));
            if (!lua_isnil(state, luaIndex) && !value)
            {
                freeTemporaryHandles();
                return luaL_error(state, "failed to create Mono string for argument %d", static_cast<int>(index + 1));
            }
            if (value)
            {
                parameterHandles[index] = resolver.CreateGCHandle(value, true);
                if (!parameterHandles[index])
                {
                    freeTemporaryHandles();
                    return luaL_error(state, "failed to pin string argument %d", static_cast<int>(index + 1));
                }
            }
            parameters[index] = value;
            continue;
        }
        else if (IsReferenceType(resolver, type, declaredKind))
        {
            MonoObject* value = lua_isnil(state, luaIndex)
                ? nullptr : LuaBridge_GetInstanceObject(state, luaIndex);
            if (value)
            {
                parameterHandles[index] = resolver.CreateGCHandle(value, true);
                if (!parameterHandles[index])
                {
                    freeTemporaryHandles();
                    return luaL_error(state, "failed to pin reference argument %d", static_cast<int>(index + 1));
                }
            }
            parameters[index] = value;
            continue;
        }
        else if (declaredKind == TYPE_VALUETYPE && kind == TYPE_VALUETYPE)
        {
            if (!luaL_testudata(state, luaIndex, LuaBridgeMT::INSTANCE))
            {
                freeTemporaryHandles();
                return luaL_error(state,
                    "argument %d requires a boxed value-type Instance",
                    static_cast<int>(index + 1));
            }
            MonoObject* boxed = LuaBridge_GetInstanceObject(state, luaIndex);
            parameterHandles[index] = resolver.CreateGCHandle(boxed, true);
            if (!parameterHandles[index])
            {
                freeTemporaryHandles();
                return luaL_error(state, "failed to pin value-type argument %d", static_cast<int>(index + 1));
            }
            parameters[index] = resolver.Unbox(boxed);
            continue;
        }
        else
        {
            freeTemporaryHandles();
            return luaL_error(state, "argument %d uses an unsupported value type",
                static_cast<int>(index + 1));
        }
        parameters[index] = slot;
    }

    // 参数编组可能分配多个托管字符串。调用前从 pinned handle 重新解析
    // 所有引用，确保传给 mono_runtime_invoke 的地址属于当前 GC 周期。
    for (size_t index = 0; index < types.size(); ++index)
    {
        if (!parameterHandles[index]) continue;
        MonoType* type = EffectiveParameterType(resolver, types[index]);
        const int declaredKind = resolver.TypeKind(type);
        const int kind = StorageTypeKind(resolver, type);
        MonoObject* target = resolver.GCHandleTarget(parameterHandles[index]);
        parameters[index] = declaredKind == TYPE_VALUETYPE && kind == TYPE_VALUETYPE
            ? resolver.Unbox(target) : target;
    }
    if (objectHandle) object = resolver.GCHandleTarget(objectHandle);

    MonoObject* exception = nullptr;
    MonoObject* result = resolver.Invoke(
        method, object, parameters.empty() ? nullptr : parameters.data(), &exception);
    freeTemporaryHandles();
    if (exception)
    {
        const std::string message = resolver.ObjectString(exception);
        return luaL_error(state, "managed exception: %s", message.empty() ? "<unknown>" : message.c_str());
    }

    MonoType* returnType = resolver.MethodReturnType(method);
    const int declaredKind = resolver.TypeKind(returnType);
    const int kind = StorageTypeKind(resolver, returnType);
    if (kind == TYPE_VOID) return 0;
    if (!result) { lua_pushnil(state); return 1; }
    if (kind == TYPE_STRING)
    {
        const std::string text = resolver.StringValue(reinterpret_cast<MonoString*>(result));
        lua_pushlstring(state, text.data(), text.size());
        return 1;
    }
    if (IsReferenceType(resolver, returnType, declaredKind))
    {
        LuaBridge_PushInstance(state, result);
        return 1;
    }
    if (declaredKind == TYPE_VALUETYPE && kind == TYPE_VALUETYPE)
    {
        LuaBridge_PushInstance(state, result);
        return 1;
    }

    void* value = resolver.Unbox(result);
    if (!value) { lua_pushnil(state); return 1; }
    switch (kind)
    {
    case TYPE_BOOLEAN: lua_pushboolean(state, *static_cast<uint8_t*>(value) != 0); break;
    case TYPE_CHAR: case TYPE_U2: lua_pushinteger(state, *static_cast<uint16_t*>(value)); break;
    case TYPE_I1: lua_pushinteger(state, *static_cast<int8_t*>(value)); break;
    case TYPE_U1: lua_pushinteger(state, *static_cast<uint8_t*>(value)); break;
    case TYPE_I2: lua_pushinteger(state, *static_cast<int16_t*>(value)); break;
    case TYPE_I4: lua_pushinteger(state, *static_cast<int32_t*>(value)); break;
    case TYPE_U4: lua_pushinteger(state, *static_cast<uint32_t*>(value)); break;
    case TYPE_I8: case TYPE_I: lua_pushinteger(state, static_cast<lua_Integer>(*static_cast<int64_t*>(value))); break;
    case TYPE_U8: case TYPE_U: lua_pushinteger(state, static_cast<lua_Integer>(*static_cast<uint64_t*>(value))); break;
    case TYPE_R4: lua_pushnumber(state, *static_cast<float*>(value)); break;
    case TYPE_R8: lua_pushnumber(state, *static_cast<double*>(value)); break;
    default: return luaL_error(state, "method returned an unsupported value type");
    }
    return 1;
}

bool LuaBridge_ReadFieldValue(
    lua_State* state, MonoObject* object, MonoClassField* field,
    bool isStatic, std::string& error)
{
    auto& resolver = MonoResolver::Instance();
    MonoType* type = resolver.FieldType(field);
    const int declaredKind = resolver.TypeKind(type);
    const int kind = StorageTypeKind(resolver, type);
    if (declaredKind == TYPE_VALUETYPE && kind == TYPE_VALUETYPE)
    {
        MonoDomain* domain = isStatic
            ? MonoRuntime::Instance().Domain() : resolver.ObjectDomain(object);
        MonoObject* boxed = resolver.FieldValueObject(
            domain, field, isStatic ? nullptr : object);
        if (!boxed) { error = "failed to box value-type field"; return false; }
        LuaBridge_PushInstance(state, boxed);
        return true;
    }
    FieldStorage value{};
    if (isStatic)
    {
        if (!resolver.ReadStaticField(MonoRuntime::Instance().Domain(), field, &value))
        {
            error = "failed to read static field";
            return false;
        }
    }
    else resolver.ReadField(object, field, &value);

    switch (kind)
    {
    case TYPE_BOOLEAN: lua_pushboolean(state, value.u1 != 0); return true;
    case TYPE_CHAR: lua_pushinteger(state, value.u2); return true;
    case TYPE_I1: lua_pushinteger(state, value.i1); return true;
    case TYPE_U1: lua_pushinteger(state, value.u1); return true;
    case TYPE_I2: lua_pushinteger(state, value.i2); return true;
    case TYPE_U2: lua_pushinteger(state, value.u2); return true;
    case TYPE_I4: lua_pushinteger(state, value.i4); return true;
    case TYPE_U4: lua_pushinteger(state, value.u4); return true;
    case TYPE_I8: lua_pushinteger(state, static_cast<lua_Integer>(value.i8)); return true;
    case TYPE_U8: lua_pushinteger(state, static_cast<lua_Integer>(value.u8)); return true;
    case TYPE_I: lua_pushinteger(state, static_cast<lua_Integer>(value.nativeInt)); return true;
    case TYPE_U: lua_pushinteger(state, static_cast<lua_Integer>(value.nativeUInt)); return true;
    case TYPE_R4: lua_pushnumber(state, value.r4); return true;
    case TYPE_R8: lua_pushnumber(state, value.r8); return true;
    case TYPE_STRING:
        if (!value.object) lua_pushnil(state);
        else
        {
            const std::string text = resolver.StringValue(reinterpret_cast<MonoString*>(value.object));
            lua_pushlstring(state, text.data(), text.size());
        }
        return true;
    default:
        if (IsReferenceType(resolver, type, kind))
        {
            LuaBridge_PushInstance(state, value.object);
            return true;
        }
        error = "unsupported Mono field type";
        return false;
    }
}

bool LuaBridge_WriteFieldValue(
    lua_State* state, int valueIndex, MonoObject* object,
    MonoClassField* field, bool isStatic, std::string& error)
{
    auto& resolver = MonoResolver::Instance();
    MonoType* type = resolver.FieldType(field);
    const int declaredKind = resolver.TypeKind(type);
    const int kind = StorageTypeKind(resolver, type);
    const bool referenceType = IsReferenceType(resolver, type, declaredKind);
    uint32_t ownerHandle = 0;
    FieldStorage value{};
    lua_Integer integer = 0;
    if (declaredKind == TYPE_VALUETYPE && kind == TYPE_VALUETYPE)
    {
        if (!luaL_testudata(state, valueIndex, LuaBridgeMT::INSTANCE))
        { error = "value-type field requires a boxed Instance"; return false; }
        MonoObject* boxed = LuaBridge_GetInstanceObject(state, valueIndex);
        MonoClass* expected = resolver.TypeClass(type);
        if (!expected || resolver.ObjectClass(boxed) != expected)
        { error = "boxed Instance type is incompatible with the field type"; return false; }
        void* unboxed = resolver.Unbox(boxed);
        if (!unboxed) { error = "failed to unbox field value"; return false; }
        if (isStatic && !resolver.WriteStaticField(
            MonoRuntime::Instance().Domain(), field, unboxed))
        { error = "failed to write static value-type field"; return false; }
        if (!isStatic) resolver.WriteField(object, field, unboxed);
        return true;
    }

    switch (kind)
    {
    case TYPE_BOOLEAN:
        if (!lua_isboolean(state, valueIndex)) { error = "field value must be a boolean"; return false; }
        value.u1 = static_cast<uint8_t>(lua_toboolean(state, valueIndex)); break;
    case TYPE_CHAR: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.u2 = static_cast<uint16_t>(integer); break;
    case TYPE_I1: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.i1 = static_cast<int8_t>(integer); break;
    case TYPE_U1: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.u1 = static_cast<uint8_t>(integer); break;
    case TYPE_I2: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.i2 = static_cast<int16_t>(integer); break;
    case TYPE_U2: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.u2 = static_cast<uint16_t>(integer); break;
    case TYPE_I4: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.i4 = static_cast<int32_t>(integer); break;
    case TYPE_U4: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.u4 = static_cast<uint32_t>(integer); break;
    case TYPE_I8: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.i8 = static_cast<int64_t>(integer); break;
    case TYPE_U8: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.u8 = static_cast<uint64_t>(integer); break;
    case TYPE_I: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.nativeInt = static_cast<intptr_t>(integer); break;
    case TYPE_U: if (!RequireIntegerForKind(state, valueIndex, kind, integer, error)) return false; value.nativeUInt = static_cast<uintptr_t>(integer); break;
    case TYPE_R4:
    case TYPE_R8:
        if (!lua_isnumber(state, valueIndex)) { error = "field value must be a number"; return false; }
        if (kind == TYPE_R4) value.r4 = static_cast<float>(lua_tonumber(state, valueIndex));
        else value.r8 = static_cast<double>(lua_tonumber(state, valueIndex));
        break;
    case TYPE_STRING:
        if (lua_isnil(state, valueIndex)) value.object = nullptr;
        else if (lua_type(state, valueIndex) == LUA_TSTRING)
        {
            if (!isStatic)
            {
                ownerHandle = resolver.CreateGCHandle(object, true);
                if (!ownerHandle)
                {
                    error = "failed to pin the field owner";
                    return false;
                }
            }
            value.object = reinterpret_cast<MonoObject*>(resolver.NewString(
                isStatic ? MonoRuntime::Instance().Domain() : resolver.ObjectDomain(object),
                lua_tostring(state, valueIndex)));
            if (ownerHandle) object = resolver.GCHandleTarget(ownerHandle);
            if (!value.object)
            {
                if (ownerHandle) resolver.FreeGCHandle(ownerHandle);
                error = "failed to create Mono string in the object's domain";
                return false;
            }
        }
        else { error = "string field value must be a string or nil"; return false; }
        break;
    default:
        if (!referenceType)
        {
            error = "unsupported Mono field type";
            return false;
        }
        if (lua_isnil(state, valueIndex)) value.object = nullptr;
        else if (luaL_testudata(state, valueIndex, LuaBridgeMT::INSTANCE))
        {
            value.object = LuaBridge_GetInstanceObject(state, valueIndex);
            MonoClass* expectedClass = resolver.TypeClass(type);
            if (expectedClass && !resolver.ObjectIsInstanceOf(value.object, expectedClass))
            {
                error = "Instance is not compatible with the field type";
                return false;
            }
        }
        else { error = "reference field value must be an Instance or nil"; return false; }
        break;
    }

    // Mono Embedding API 在这里有一个容易混淆但非常重要的不对称：
    // ·值类型传原始值缓冲区的地址；
    // ·引用类型直接传 MonoObject*，不能再传 MonoObject**。
    // 读取引用字段仍然需要 MonoObject** 作为输出位置，不能把两条规则合并。
    void* rawValue = referenceType
        ? static_cast<void*>(value.object)
        : static_cast<void*>(&value);
    if (isStatic)
    {
        if (!resolver.WriteStaticField(
            MonoRuntime::Instance().Domain(), field, rawValue))
        {
            if (ownerHandle) resolver.FreeGCHandle(ownerHandle);
            error = "failed to write static field";
            return false;
        }
    }
    else resolver.WriteField(object, field, rawValue);
    if (ownerHandle) resolver.FreeGCHandle(ownerHandle);
    return true;
}

bool LuaBridge_PushRawValue(
    lua_State* state, MonoType* type, void* address, std::string& error)
{
    if (!type || !address) { error = "value address is null"; return false; }
    auto& resolver = MonoResolver::Instance();
    const int declaredKind = resolver.TypeKind(type);
    const int kind = StorageTypeKind(resolver, type);
    switch (kind)
    {
    case TYPE_BOOLEAN: lua_pushboolean(state, *static_cast<uint8_t*>(address) != 0); return true;
    case TYPE_CHAR: case TYPE_U2: lua_pushinteger(state, *static_cast<uint16_t*>(address)); return true;
    case TYPE_I1: lua_pushinteger(state, *static_cast<int8_t*>(address)); return true;
    case TYPE_U1: lua_pushinteger(state, *static_cast<uint8_t*>(address)); return true;
    case TYPE_I2: lua_pushinteger(state, *static_cast<int16_t*>(address)); return true;
    case TYPE_I4: lua_pushinteger(state, *static_cast<int32_t*>(address)); return true;
    case TYPE_U4: lua_pushinteger(state, *static_cast<uint32_t*>(address)); return true;
    case TYPE_I8: case TYPE_I: lua_pushinteger(state, static_cast<lua_Integer>(*static_cast<int64_t*>(address))); return true;
    case TYPE_U8: case TYPE_U: lua_pushinteger(state, static_cast<lua_Integer>(*static_cast<uint64_t*>(address))); return true;
    case TYPE_R4: lua_pushnumber(state, *static_cast<float*>(address)); return true;
    case TYPE_R8: lua_pushnumber(state, *static_cast<double*>(address)); return true;
    case TYPE_STRING:
    {
        MonoString* string = *static_cast<MonoString**>(address);
        if (!string) lua_pushnil(state);
        else { const std::string text = resolver.StringValue(string); lua_pushlstring(state, text.data(), text.size()); }
        return true;
    }
    default:
        if (IsReferenceType(resolver, type, declaredKind))
        {
            LuaBridge_PushInstance(state, *static_cast<MonoObject**>(address));
            return true;
        }
        if (declaredKind == TYPE_VALUETYPE && kind == TYPE_VALUETYPE)
        {
            MonoObject* boxed = resolver.Box(
                MonoRuntime::Instance().Domain(), resolver.TypeClass(type), address);
            if (!boxed) { error = "failed to box array element"; return false; }
            LuaBridge_PushInstance(state, boxed);
            return true;
        }
        error = "unsupported container element type";
        return false;
    }
}

bool LuaBridge_WriteArrayValue(
    lua_State* state, int valueIndex, MonoArray* array, MonoType* type,
    uintptr_t index, std::string& error)
{
    if (!array || !type) { error = "array or element type is null"; return false; }
    auto& resolver = MonoResolver::Instance();
    MonoClass* arrayClass = resolver.ObjectClass(reinterpret_cast<MonoObject*>(array));
    const int elementSize = resolver.ArrayElementSize(arrayClass);
    if (elementSize <= 0) { error = "array element size is invalid"; return false; }
    void* address = resolver.ArrayAddress(array, elementSize, index);
    if (!address) { error = "array element address is null"; return false; }
    const int declaredKind = resolver.TypeKind(type);
    const int kind = StorageTypeKind(resolver, type);
#define WRITE_INTEGER(T) do { \
    if (!lua_isinteger(state, valueIndex)) { error = "array value must be an integer"; return false; } \
    const lua_Integer integer = lua_tointeger(state, valueIndex); \
    if (!IntegerFits(kind, integer)) { error = "integer value is outside the array element type range"; return false; } \
    *static_cast<T*>(address) = static_cast<T>(integer); return true; \
} while (0)
    switch (kind)
    {
    case TYPE_BOOLEAN:
        if (!lua_isboolean(state, valueIndex)) { error = "array value must be a boolean"; return false; }
        *static_cast<uint8_t*>(address) = static_cast<uint8_t>(lua_toboolean(state, valueIndex)); return true;
    case TYPE_CHAR: case TYPE_U2: WRITE_INTEGER(uint16_t);
    case TYPE_I1: WRITE_INTEGER(int8_t); case TYPE_U1: WRITE_INTEGER(uint8_t);
    case TYPE_I2: WRITE_INTEGER(int16_t); case TYPE_I4: WRITE_INTEGER(int32_t);
    case TYPE_U4: WRITE_INTEGER(uint32_t); case TYPE_I8: case TYPE_I: WRITE_INTEGER(int64_t);
    case TYPE_U8: case TYPE_U: WRITE_INTEGER(uint64_t);
    case TYPE_R4:
        if (!lua_isnumber(state, valueIndex)) { error = "array value must be a number"; return false; }
        *static_cast<float*>(address) = static_cast<float>(lua_tonumber(state, valueIndex)); return true;
    case TYPE_R8:
        if (!lua_isnumber(state, valueIndex)) { error = "array value must be a number"; return false; }
        *static_cast<double*>(address) = lua_tonumber(state, valueIndex); return true;
    default: break;
    }
#undef WRITE_INTEGER
    if (declaredKind == TYPE_VALUETYPE && kind == TYPE_VALUETYPE)
    {
        if (!luaL_testudata(state, valueIndex, LuaBridgeMT::INSTANCE))
        { error = "value-type array element requires a boxed Instance"; return false; }
        MonoObject* boxed = LuaBridge_GetInstanceObject(state, valueIndex);
        MonoClass* expected = resolver.TypeClass(type);
        if (!expected || resolver.ObjectClass(boxed) != expected)
        { error = "boxed Instance type is incompatible with array element type"; return false; }
        void* unboxed = resolver.Unbox(boxed);
        if (!unboxed) { error = "failed to unbox array element"; return false; }
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
    if (kind == TYPE_STRING)
    {
        if (!lua_isnil(state, valueIndex))
        {
            if (lua_type(state, valueIndex) != LUA_TSTRING) { error = "array value must be a string or nil"; return false; }
            const uint32_t arrayHandle = resolver.CreateGCHandle(
                reinterpret_cast<MonoObject*>(array), true);
            if (!arrayHandle) { error = "failed to pin the Mono array"; return false; }
            value = reinterpret_cast<MonoObject*>(resolver.NewString(
                resolver.ObjectDomain(reinterpret_cast<MonoObject*>(array)),
                lua_tostring(state, valueIndex)));
            array = reinterpret_cast<MonoArray*>(resolver.GCHandleTarget(arrayHandle));
            address = resolver.ArrayAddress(array, elementSize, index);
            resolver.FreeGCHandle(arrayHandle);
            if (!value || !array || !address)
            {
                error = "failed to create the Mono string array value";
                return false;
            }
        }
    }
    else if (!lua_isnil(state, valueIndex))
    {
        if (!luaL_testudata(state, valueIndex, LuaBridgeMT::INSTANCE)) { error = "array value must be an Instance or nil"; return false; }
        value = LuaBridge_GetInstanceObject(state, valueIndex);
        MonoClass* expected = resolver.TypeClass(type);
        if (expected && !resolver.ObjectIsInstanceOf(value, expected)) { error = "Instance type is incompatible with array element type"; return false; }
    }
    if (!resolver.SetArrayReference(array, address, value))
    {
        error = "this Mono runtime cannot write reference array elements";
        return false;
    }
    return true;
}
