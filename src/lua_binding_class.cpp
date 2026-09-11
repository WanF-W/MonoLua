/**
 * lua_binding_class.cpp — Class userdata 公开 API
 * 提供类型元数据查询、方法/字段查找、对象创建、Unity 对象查找以及
 * 静态字段读写。所有对象结果都交给 Instance 的强 GC handle 管理。
 *
 * MonoClass 属于运行时元数据，userdata 保存借用指针；所有查询均通过
 * Mono Embedding API，不读取 MonoClass 内部布局。
 */
#include "lua_binding_internal.h"
#include "lua_dump.h"
#include "mono_metadata.h"
#include "mono_resolver.h"
#include "mono_runtime.h"
#include "unity_object_query.h"
#include "lua_engine.h"
#include <sstream>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    std::string NormalizeTypeName(const std::string& input)
    {
        static const std::map<std::string, std::string> aliases = {
            {"bool", "System.Boolean"},  {"byte", "System.Byte"},     {"sbyte", "System.SByte"},
            {"char", "System.Char"},     {"short", "System.Int16"},   {"ushort", "System.UInt16"},
            {"int", "System.Int32"},     {"uint", "System.UInt32"},   {"long", "System.Int64"},
            {"ulong", "System.UInt64"},  {"float", "System.Single"},  {"double", "System.Double"},
            {"string", "System.String"}, {"object", "System.Object"}, {"void", "System.Void"}};

        // 与 Il2CppLua 保持一致：数组后缀保留，只规范化元素类型别名。
        if (input.size() > 2 && input.compare(input.size() - 2, 2, "[]") == 0)
        {
            return NormalizeTypeName(input.substr(0, input.size() - 2)) + "[]";
        }

        const auto found = aliases.find(input);
        return found != aliases.end() ? found->second : input;
    }

    bool TypeNameMatches(const std::string& actual, const std::string& requestedInput)
    {
        const std::string requested = NormalizeTypeName(requestedInput);
        if (actual == requested) return true;

        // 用户已经提供命名空间或嵌套类型路径时必须完整匹配；只提供末级
        // 类名时，允许匹配 Mono 返回的 Namespace.Type / Outer+Nested。
        if (requested.find_first_of("./+") != std::string::npos) return false;
        const size_t separator = actual.find_last_of("./+");
        return separator != std::string::npos && actual.substr(separator + 1) == requested;
    }

    std::string ClassFullName(MonoClass* klass)
    {
        auto& resolver = MonoResolver::Instance();
        const char* nameSpace = resolver.ClassNamespace(klass);
        const char* name = resolver.ClassName(klass);
        std::string result = nameSpace && *nameSpace ? std::string(nameSpace) + '.' : std::string{};
        result += name ? name : "<invalid>";
        return result;
    }

    int Class_GetName(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const char* name = MonoResolver::Instance().ClassName(userdata->klass);
        lua_pushstring(state, name ? name : "");
        return 1;
    }

    int Class_GetNamespace(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const char* nameSpace = MonoResolver::Instance().ClassNamespace(userdata->klass);
        lua_pushstring(state, nameSpace ? nameSpace : "");
        return 1;
    }

    int Class_GetFullName(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const std::string fullName = ClassFullName(userdata->klass);
        lua_pushlstring(state, fullName.data(), fullName.size());
        return 1;
    }

    int Class_GetAssembly(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        MonoAssemblyInfo assembly;
        MonoImage* image = MonoResolver::Instance().ClassImage(userdata->klass);
        if (!MonoRuntime::Instance().FindAssemblyByImage(image, assembly))
        {
            lua_pushnil(state);
            return 1;
        }

        LuaBridge_PushAssembly(state, assembly);
        return 1;
    }

    int Class_GetParent(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        LuaBridge_PushClass(state, MonoResolver::Instance().ClassParent(userdata->klass));
        return 1;
    }

    int Class_IsValueType(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        lua_pushboolean(state, MonoResolver::Instance().ClassIsValueType(userdata->klass));
        return 1;
    }

    int Class_IsEnum(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        lua_pushboolean(state, MonoResolver::Instance().ClassIsEnum(userdata->klass));
        return 1;
    }

    int Class_GetInstanceSize(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const int32_t size = MonoResolver::Instance().ClassInstanceSize(userdata->klass);
        if (size <= 0)
        {
            lua_pushnil(state);
            return 1;
        }

        lua_pushinteger(state, static_cast<lua_Integer>(size));
        return 1;
    }

    int Class_GetAddress(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(userdata->klass)));
        return 1;
    }

    int Class_GetMethod(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const char* wantedName = luaL_checkstring(state, 2);
        const int wantedCount = lua_gettop(state) - 2;
        auto& resolver = MonoResolver::Instance();

        for (int index = 0; index < wantedCount; ++index) luaL_checkstring(state, index + 3);
        const bool constructor = strcmp(wantedName, ".ctor") == 0 || strcmp(wantedName, ".cctor") == 0;
        for (MonoClass* current = userdata->klass; current; current = constructor ? nullptr : resolver.ClassParent(current))
        for (MonoMethod* method : resolver.EnumerateMethods(current))
        {
            const char* name = resolver.MethodName(method);
            if (!name || strcmp(name, wantedName) != 0) continue;
            // 未提供参数类型时保持便捷查找语义，返回第一个同名方法；存在
            // 重载时调用方应显式传入类型列表获得可预测结果。
            if (wantedCount == 0)
            {
                LuaBridge_PushMethod(state, method);
                return 1;
            }
            const auto parameters = resolver.MethodParameterTypes(method);
            if (static_cast<int>(parameters.size()) != wantedCount) continue;

            bool matched = true;
            for (int index = 0; index < wantedCount; ++index)
            {
                const char* requested = luaL_checkstring(state, index + 3);
                if (!TypeNameMatches(parameters[static_cast<size_t>(index)], requested))
                {
                    matched = false;
                    break;
                }
            }
            if (matched)
            {
                LuaBridge_PushMethod(state, method);
                return 1;
            }
        }
        lua_pushnil(state);
        return 1;
    }

    int Class_GetMethods(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const auto methods = MonoResolver::Instance().EnumerateMethods(userdata->klass);
        lua_createtable(state, static_cast<int>(methods.size()), 0);
        for (size_t index = 0; index < methods.size(); ++index)
        {
            LuaBridge_PushMethod(state, methods[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        return 1;
    }

    int Class_GetField(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        LuaBridge_PushField(state,
                            MonoResolver::Instance().FindField(userdata->klass, luaL_checkstring(state, 2)));
        return 1;
    }

    int Class_GetFields(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const auto fields = MonoResolver::Instance().EnumerateFields(userdata->klass);
        lua_createtable(state, static_cast<int>(fields.size()), 0);
        for (size_t index = 0; index < fields.size(); ++index)
        {
            LuaBridge_PushField(state, fields[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        return 1;
    }

    int Class_ReadStaticField(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const char* name = luaL_checkstring(state, 2);
        auto& resolver = MonoResolver::Instance();
        MonoClassField* field = resolver.FindFieldInHierarchy(userdata->klass, name);
        if (!field) return LuaEngine::RaiseBridgeError(state, "field not found: %s", name);
        if ((resolver.FieldFlags(field) & mono_metadata::FIELD_ATTRIBUTE_STATIC) == 0)
            return LuaEngine::RaiseBridgeError(state, "field is not static: %s", name);

        std::string error;
        if (!LuaBridge_ReadFieldValue(state, nullptr, field, true, error))
            return LuaEngine::RaiseBridgeError(state, "%s: %s", name, error.c_str());
        return 1;
    }

    int Class_WriteStaticField(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const char* name = luaL_checkstring(state, 2);
        auto& resolver = MonoResolver::Instance();
        MonoClassField* field = resolver.FindFieldInHierarchy(userdata->klass, name);
        if (!field) return LuaEngine::RaiseBridgeError(state, "field not found: %s", name);
        const uint32_t flags = resolver.FieldFlags(field);
        if ((flags & mono_metadata::FIELD_ATTRIBUTE_STATIC) == 0)
            return LuaEngine::RaiseBridgeError(state, "field is not static: %s", name);
        if ((flags & mono_metadata::FIELD_ATTRIBUTE_LITERAL) != 0)
            return LuaEngine::RaiseBridgeError(state, "const field cannot be written: %s", name);

        std::string error;
        if (!LuaBridge_WriteFieldValue(state, 3, nullptr, field, true, error))
            return LuaEngine::RaiseBridgeError(state, "%s: %s", name, error.c_str());
        return 0;
    }

    int Class_StaticCall(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const char* name = luaL_checkstring(state, 2);
        MonoMethod* matched = LuaBridge_FindBestMethod(state, userdata->klass, name, 3, true);
        if (matched) return LuaBridge_InvokeMethod(state, matched, nullptr, 3);
        return LuaEngine::RaiseBridgeError(state, "static method not found or arguments are incompatible: %s", name);
    }

    int Class_FindUnityObjects(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        mono::GCHandles handles;
        std::string error;
        const bool found = UnityObjectQuery::FindObjectsOfType(userdata->klass, handles, error);
        LuaEngine::Instance().CheckHealthy();
        if (!found)
        {
            lua_pushnil(state);
            lua_pushlstring(state, error.data(), error.size());
            return 2;
        }

        lua_createtable(state, static_cast<int>(handles.values.size()), 0);
        for (size_t index = 0; index < handles.values.size(); ++index)
        {
            // handle 的所有权在这里转交给 Instance userdata，由其 __gc 释放。
            LuaBridge_PushInstanceHandle(state, handles.values[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        return 1;
    }

    int Class_Alloc(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        MonoObject* object =
            MonoResolver::Instance().NewObject(MonoRuntime::Instance().Domain(), userdata->klass);
        if (!object) return LuaEngine::RaiseBridgeError(state, "failed to allocate Mono object");
        LuaBridge_PushInstance(state, object);
        return 1;
    }

    int Class_New(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        auto& resolver = MonoResolver::Instance();
        MonoMethod* constructor = LuaBridge_FindBestMethod(state, userdata->klass, ".ctor", 2, false);
        if (!constructor && (lua_gettop(state) != 1 || !resolver.ClassIsValueType(userdata->klass)))
            return LuaEngine::RaiseBridgeError(state, "matching constructor was not found");
        MonoObject* object = resolver.NewObject(MonoRuntime::Instance().Domain(), userdata->klass);
        if (!object) return LuaEngine::RaiseBridgeError(state, "failed to allocate Mono object");

        // 构造参数编组可能创建字符串并触发 Mono GC。先把新对象交给
        // Instance 的强 handle，再从 handle 取得当前地址调用构造函数，
        // 避免构造完成后继续使用分配时保存的裸地址。
        LuaBridge_PushInstance(state, object);
        lua_insert(state, 2);
        object = LuaBridge_GetInstanceObject(state, 2);
        const int results = constructor ? LuaBridge_InvokeMethod(state, constructor, object, 3) : 0;
        if (results != 0) lua_pop(state, results);
        lua_settop(state, 2);
        return 1;
    }

    int Class_NewArray(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const lua_Integer length = luaL_checkinteger(state, 2);
        if (length < 0) return LuaEngine::RaiseBridgeError(state, "array length cannot be negative");
        if (length > INT32_MAX) return LuaEngine::RaiseBridgeError(state, "array length exceeds the Lua container limit");
        MonoArray* array = MonoResolver::Instance().NewArray(MonoRuntime::Instance().Domain(),
                                                             userdata->klass, static_cast<uintptr_t>(length));
        if (!array) return LuaEngine::RaiseBridgeError(state, "failed to create Mono array");
        LuaBridge_PushInstance(state, reinterpret_cast<MonoObject*>(array));
        return 1;
    }

    int Class_Dump(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        auto& resolver = MonoResolver::Instance();
        LuaDumpStream output;
        output << "Class: " << ClassFullName(userdata->klass) << '\n';
        const char* assembly = resolver.ImageName(resolver.ClassImage(userdata->klass));
        output << "Assembly: " << (assembly ? assembly : "?") << '\n';
        output << "Kind: "
               << (resolver.ClassIsEnum(userdata->klass)        ? "enum"
                   : resolver.ClassIsValueType(userdata->klass) ? "struct"
                                                                : "class")
               << '\n';
        MonoClass* parent = resolver.ClassParent(userdata->klass);
        output << "Parent: " << (parent ? ClassFullName(parent) : "nil") << '\n';
        output << "Address: 0x" << std::hex << std::uppercase << reinterpret_cast<uintptr_t>(userdata->klass)
               << std::dec << "\nInstance Size: " << resolver.ClassInstanceSize(userdata->klass);
        const auto fields = resolver.EnumerateFields(userdata->klass);
        const auto methods = resolver.EnumerateMethods(userdata->klass);
        output << "\n\nFields (" << fields.size() << "):\n";
        for (MonoClassField* field : fields)
        {
            if (output.Full()) { output << " "; break; }
            output << "  " << resolver.FieldSignature(field) << '\n';
        }
        output << "\nMethods (" << methods.size() << "):\n";
        for (MonoMethod* method : methods)
        {
            if (output.Full()) { output << " "; break; }
            output << "  " << resolver.MethodSignature(method) << '\n';
        }
        std::string text = output.str();
        if (!text.empty() && text.back() == '\n') text.pop_back();
        lua_getglobal(state, "print");
        lua_pushlstring(state, text.data(), text.size());
        lua_call(state, 1, 0);
        return 0;
    }

    int Class_ToString(lua_State* state)
    {
        const LuaClassUD* userdata = LuaBridge_CheckClass(state, 1);
        const std::string fullName = ClassFullName(userdata->klass);
        lua_pushfstring(state, "Class: %s @ %p", fullName.c_str(), static_cast<void*>(userdata->klass));
        return 1;
    }
} // namespace

const luaL_Reg* LuaBinding_GetClassMethods()
{
    static const luaL_Reg methods[] = {{"get_name", Class_GetName},
                                       {"get_namespace", Class_GetNamespace},
                                       {"get_full_name", Class_GetFullName},
                                       {"get_assembly", Class_GetAssembly},
                                       {"get_parent", Class_GetParent},
                                       {"is_value_type", Class_IsValueType},
                                       {"is_enum", Class_IsEnum},
                                       {"get_instance_size", Class_GetInstanceSize},
                                       {"get_address", Class_GetAddress},
                                       {"get_method", Class_GetMethod},
                                       {"get_methods", Class_GetMethods},
                                       {"get_field", Class_GetField},
                                       {"get_fields", Class_GetFields},
                                       {"read_static_field", Class_ReadStaticField},
                                       {"write_static_field", Class_WriteStaticField},
                                       {"static_call", Class_StaticCall},
                                       {"find_unity_objects", Class_FindUnityObjects},
                                       {"alloc", Class_Alloc},
                                       {"new", Class_New},
                                       {"new_array", Class_NewArray},
                                       {"dump", Class_Dump},
                                       {"__tostring", Class_ToString},
                                       {nullptr, nullptr}};
    return methods;
}
