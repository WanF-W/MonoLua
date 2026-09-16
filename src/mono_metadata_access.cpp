// Metadata queries and signature formatting; no Lua dependency.
#include "mono_resolver.h"
#include "mono_metadata.h"
#include "mono_native_call.h"
#include <sstream>
using namespace mono_native;

namespace
{
    std::string MemberAccess(uint32_t flags)
    {
        switch (flags & 0x0007)
        {
        case 0x0001:
            return "private";
        case 0x0002:
            return "private protected";
        case 0x0003:
            return "internal";
        case 0x0004:
            return "protected";
        case 0x0005:
            return "protected internal";
        case 0x0006:
            return "public";
        default:
            return "private scope";
        }
    }

}

void MonoResolver::EnumerateAssemblies(MonoAssemblyForeachCallback callback, void* data) const
{
    if (m_assemblyForeach && callback) SafeMonoCallVoid(m_assemblyForeach, callback, data);
}

MonoImage* MonoResolver::AssemblyImage(MonoAssembly* assembly) const
{
    if (!m_assemblyGetImage || !assembly) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_assemblyGetImage, nativeFault, assembly);
}

const char* MonoResolver::ImageName(MonoImage* image) const
{
    if (!m_imageGetName || !image) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_imageGetName, nativeFault, image);
}

MonoClass* MonoResolver::FindClass(MonoImage* image, const char* ns, const char* name) const
{
    if (!m_classFromName || !image || !ns || !name) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_classFromName, nativeFault, image, ns, name);
}

std::vector<MonoClass*> MonoResolver::EnumerateClasses(MonoImage* image) const
{
    std::vector<MonoClass*> classes;
    if (!image || !m_imageGetTableInfo || !m_tableInfoGetRows || !m_classGet) return classes;

    // ECMA-335 元数据表编号 2 是 TypeDef；Class token 的高字节 0x02，
    // 低 24 位是从 1 开始的行号。无法解析的特殊条目直接跳过。
    constexpr int MONO_TABLE_TYPEDEF = 2;
    constexpr uint32_t MONO_TOKEN_TYPE_DEF = 0x02000000;
    bool nativeFault = false;
    const MonoTableInfo* table = SafeMonoCall(m_imageGetTableInfo, nativeFault, image, MONO_TABLE_TYPEDEF);
    const int rows = table && !nativeFault ? SafeMonoCall(m_tableInfoGetRows, nativeFault, table) : 0;
    if (rows <= 0) return classes;

    classes.reserve(static_cast<size_t>(rows));
    for (int row = 1; row <= rows; ++row)
    {
        MonoClass* klass = SafeMonoCall(m_classGet, nativeFault, image,
                                        MONO_TOKEN_TYPE_DEF | static_cast<uint32_t>(row));
        if (nativeFault) break;
        if (klass) classes.push_back(klass);
    }
    return classes;
}

const char* MonoResolver::ClassName(MonoClass* klass) const
{
    if (!m_classGetName || !klass) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_classGetName, nativeFault, klass);
}

const char* MonoResolver::ClassNamespace(MonoClass* klass) const
{
    if (!m_classGetNamespace || !klass) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_classGetNamespace, nativeFault, klass);
}

MonoClass* MonoResolver::ClassParent(MonoClass* klass) const
{
    if (!m_classGetParent || !klass) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_classGetParent, nativeFault, klass);
}

MonoImage* MonoResolver::ClassImage(MonoClass* klass) const
{
    if (!m_classGetImage || !klass) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_classGetImage, nativeFault, klass);
}

bool MonoResolver::ClassIsValueType(MonoClass* klass) const
{
    if (!m_classIsValueType || !klass) return false;
    bool nativeFault = false;
    const int result = SafeMonoCall(m_classIsValueType, nativeFault, klass);
    return !nativeFault && result != 0;
}

bool MonoResolver::ClassIsEnum(MonoClass* klass) const
{
    if (!m_classIsEnum || !klass) return false;
    bool nativeFault = false;
    const int result = SafeMonoCall(m_classIsEnum, nativeFault, klass);
    return !nativeFault && result != 0;
}

int32_t MonoResolver::ClassInstanceSize(MonoClass* klass) const
{
    if (!m_classInstanceSize || !klass) return 0;
    bool nativeFault = false;
    return SafeMonoCall(m_classInstanceSize, nativeFault, klass);
}

std::vector<MonoMethod*> MonoResolver::EnumerateMethods(MonoClass* klass) const
{
    std::vector<MonoMethod*> methods;
    if (!klass || !m_classGetMethods) return methods;
    if (m_classNumMethods)
    {
        bool nativeFault = false;
        const int count = SafeMonoCall(m_classNumMethods, nativeFault, klass);
        if (count > 0) methods.reserve(static_cast<size_t>(count));
        if (nativeFault) return methods;
    }
    void* iterator = nullptr;
    bool nativeFault = false;
    while (MonoMethod* method = SafeMonoCall(m_classGetMethods, nativeFault, klass, &iterator))
    {
        if (nativeFault) break;
        if (!m_methodGetClass || SafeMonoCall(m_methodGetClass, nativeFault, method) == klass)
            methods.push_back(method);
        if (nativeFault) break;
    }
    return methods;
}

std::vector<MonoClassField*> MonoResolver::EnumerateFields(MonoClass* klass) const
{
    std::vector<MonoClassField*> fields;
    if (!klass || !m_classGetFields) return fields;
    if (m_classNumFields)
    {
        bool nativeFault = false;
        const int count = SafeMonoCall(m_classNumFields, nativeFault, klass);
        if (count > 0) fields.reserve(static_cast<size_t>(count));
        if (nativeFault) return fields;
    }
    void* iterator = nullptr;
    bool nativeFault = false;
    while (MonoClassField* field = SafeMonoCall(m_classGetFields, nativeFault, klass, &iterator))
    {
        if (nativeFault) break;
        if (!m_fieldGetParent || SafeMonoCall(m_fieldGetParent, nativeFault, field) == klass)
            fields.push_back(field);
        if (nativeFault) break;
    }
    return fields;
}

MonoClassField* MonoResolver::FindField(MonoClass* klass, const char* name) const
{
    if (!m_classGetFieldFromName || !klass || !name) return nullptr;
    bool nativeFault = false;
    MonoClassField* field = SafeMonoCall(m_classGetFieldFromName, nativeFault, klass, name);
    // mono_class_get_field_from_name 在部分版本会沿父类查找；MonoLua 的
    // Class API 只允许当前声明类，防止同名隐藏字段产生不确定结果。
    return field && (!m_fieldGetParent || SafeMonoCall(m_fieldGetParent, nativeFault, field) == klass) &&
                   !nativeFault
               ? field
               : nullptr;
}

MonoClassField* MonoResolver::FindFieldInHierarchy(MonoClass* klass, const char* name) const
{
    for (MonoClass* current = klass; current; current = ClassParent(current))
        if (MonoClassField* field = FindField(current, name)) return field;
    return nullptr;
}

const char* MonoResolver::MethodName(MonoMethod* method) const
{
    if (!m_methodGetName || !method) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_methodGetName, nativeFault, method);
}

MonoClass* MonoResolver::MethodClass(MonoMethod* method) const
{
    if (!m_methodGetClass || !method) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_methodGetClass, nativeFault, method);
}

std::vector<std::string> MonoResolver::MethodParameterTypes(MonoMethod* method) const
{
    std::vector<std::string> types;
    if (!m_methodSignature || !method) return types;
    bool nativeFault = false;
    MonoMethodSignature* signature = SafeMonoCall(m_methodSignature, nativeFault, method);
    if (!signature || !m_signatureGetParams || !m_typeGetName) return types;

    void* iterator = nullptr;
    while (MonoType* type = SafeMonoCall(m_signatureGetParams, nativeFault, signature, &iterator))
    {
        if (nativeFault) break;
        char* rawName = SafeMonoCall(m_typeGetName, nativeFault, type);
        if (nativeFault) break;
        types.emplace_back(rawName ? rawName : "<unknown>");
        if (rawName && m_monoFree) SafeMonoCallVoid(m_monoFree, rawName);
    }
    return types;
}

std::vector<MonoType*> MonoResolver::MethodParameters(MonoMethod* method) const
{
    std::vector<MonoType*> types;
    if (!m_methodSignature || !method) return types;
    bool nativeFault = false;
    MonoMethodSignature* signature = SafeMonoCall(m_methodSignature, nativeFault, method);
    if (!signature || !m_signatureGetParams) return types;
    void* iterator = nullptr;
    while (MonoType* type = SafeMonoCall(m_signatureGetParams, nativeFault, signature, &iterator))
    {
        if (nativeFault) break;
        types.push_back(type);
    }
    return types;
}

MonoType* MonoResolver::MethodReturnType(MonoMethod* method) const
{
    if (!m_methodSignature || !m_signatureGetReturnType || !method) return nullptr;
    bool nativeFault = false;
    MonoMethodSignature* signature = SafeMonoCall(m_methodSignature, nativeFault, method);
    return signature && !nativeFault ? SafeMonoCall(m_signatureGetReturnType, nativeFault, signature) : nullptr;
}

uint32_t MonoResolver::MethodFlags(MonoMethod* method) const
{
    if (!m_methodGetFlags || !method) return 0;
    bool nativeFault = false;
    return SafeMonoCall(m_methodGetFlags, nativeFault, method, nullptr);
}

uint32_t MonoResolver::MethodImplementationFlags(MonoMethod* method) const
{
    uint32_t flags = 0;
    if (m_methodGetFlags && method)
    {
        bool nativeFault = false;
        SafeMonoCall(m_methodGetFlags, nativeFault, method, &flags);
    }
    return flags;
}

MonoClass* MonoResolver::ElementClass(MonoClass* arrayClass) const
{
    if (!m_classGetElementClass || !arrayClass) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_classGetElementClass, nativeFault, arrayClass);
}

int MonoResolver::ClassRank(MonoClass* klass) const
{
    if (!m_classGetRank || !klass) return -1;
    bool nativeFault = false;
    return SafeMonoCall(m_classGetRank, nativeFault, klass);
}

int MonoResolver::ArrayElementSize(MonoClass* arrayClass) const
{
    if (!m_arrayElementSize || !arrayClass) return 0;
    bool nativeFault = false;
    return SafeMonoCall(m_arrayElementSize, nativeFault, arrayClass);
}

std::string MonoResolver::TypeDisplayName(MonoType* type) const
{
    bool nativeFault = false;
    char* raw = type && m_typeGetName ? SafeMonoCall(m_typeGetName, nativeFault, type) : nullptr;
    std::string name = raw ? raw : "<unknown>";
    if (raw && m_monoFree) SafeMonoCallVoid(m_monoFree, raw);
    return name;
}

std::string MonoResolver::MethodSignature(MonoMethod* method) const
{
    if (!method) return {};
    bool nativeFault = false;
    MonoMethodSignature* signature = m_methodSignature ? SafeMonoCall(m_methodSignature, nativeFault, method) : nullptr;
    MonoType* returnType =
        signature && m_signatureGetReturnType ? SafeMonoCall(m_signatureGetReturnType, nativeFault, signature) : nullptr;
    char* rawReturn = returnType && m_typeGetName ? SafeMonoCall(m_typeGetName, nativeFault, returnType) : nullptr;

    std::ostringstream output;
    uint32_t implementationFlags = 0;
    const uint32_t flags = m_methodGetFlags ? SafeMonoCall(m_methodGetFlags, nativeFault, method, &implementationFlags) : 0;
    output << MemberAccess(flags) << ' ';
    if ((flags & mono_metadata::METHOD_ATTRIBUTE_STATIC) != 0) output << "static ";
    if ((flags & 0x0400) != 0)
        output << "abstract ";
    else if ((flags & 0x0040) != 0)
        output << "virtual ";
    if ((flags & 0x0020) != 0) output << "final ";
    output << (rawReturn ? rawReturn : "<unknown>") << ' ';
    if (rawReturn && m_monoFree) SafeMonoCallVoid(m_monoFree, rawReturn);
    MonoClass* klass = MethodClass(method);
    const char* nameSpace = ClassNamespace(klass);
    if (nameSpace && *nameSpace) output << nameSpace << '.';
    output << (ClassName(klass) ? ClassName(klass) : "<unknown>") << '.';
    output << (MethodName(method) ? MethodName(method) : "<unknown>") << '(';
    const auto parameters = MethodParameterTypes(method);
    std::vector<const char*> names(parameters.size(), nullptr);
    if (m_methodGetParamNames && !names.empty()) SafeMonoCallVoid(m_methodGetParamNames, method, names.data());
    for (size_t index = 0; index < parameters.size(); ++index)
    {
        if (index != 0) output << ", ";
        output << parameters[index] << ' ';
        if (names[index] && *names[index])
            output << names[index];
        else
            output << "arg" << index + 1;
    }
    output << ')';
    return output.str();
}

const char* MonoResolver::FieldName(MonoClassField* field) const
{
    if (!m_fieldGetName || !field) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_fieldGetName, nativeFault, field);
}

MonoClass* MonoResolver::FieldClass(MonoClassField* field) const
{
    if (!m_fieldGetParent || !field) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_fieldGetParent, nativeFault, field);
}

MonoType* MonoResolver::FieldType(MonoClassField* field) const
{
    if (!m_fieldGetType || !field) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_fieldGetType, nativeFault, field);
}

uint32_t MonoResolver::FieldFlags(MonoClassField* field) const
{
    if (!m_fieldGetFlags || !field) return 0;
    bool nativeFault = false;
    return SafeMonoCall(m_fieldGetFlags, nativeFault, field);
}

std::string MonoResolver::FieldSignature(MonoClassField* field) const
{
    if (!field) return {};
    bool nativeFault = false;
    MonoType* type = m_fieldGetType ? SafeMonoCall(m_fieldGetType, nativeFault, field) : nullptr;
    char* rawType = type && m_typeGetName ? SafeMonoCall(m_typeGetName, nativeFault, type) : nullptr;
    MonoClass* klass = FieldClass(field);

    std::ostringstream output;
    const uint32_t flags = m_fieldGetFlags ? SafeMonoCall(m_fieldGetFlags, nativeFault, field) : 0;
    output << MemberAccess(flags) << ' ';
    if ((flags & mono_metadata::FIELD_ATTRIBUTE_STATIC) != 0) output << "static ";
    if ((flags & mono_metadata::FIELD_ATTRIBUTE_LITERAL) != 0)
        output << "const ";
    else if ((flags & mono_metadata::FIELD_ATTRIBUTE_INIT_ONLY) != 0)
        output << "readonly ";
    output << (rawType ? rawType : "<unknown>") << ' ';
    if (rawType && m_monoFree) SafeMonoCallVoid(m_monoFree, rawType);
    const char* nameSpace = ClassNamespace(klass);
    if (nameSpace && *nameSpace) output << nameSpace << '.';
    output << (ClassName(klass) ? ClassName(klass) : "<unknown>") << '.';
    output << (FieldName(field) ? FieldName(field) : "<unknown>");
    return output.str();
}

int32_t MonoResolver::FieldOffset(MonoClassField* field) const
{
    // 静态字段没有相对于实例对象的有效偏移，公开 API 按约定返回 nil。
    if (!field || !m_fieldGetFlags) return -1;
    bool nativeFault = false;
    if ((SafeMonoCall(m_fieldGetFlags, nativeFault, field) & mono_metadata::FIELD_ATTRIBUTE_STATIC) != 0)
        return -1;
    return m_fieldGetOffset && !nativeFault ? static_cast<int32_t>(SafeMonoCall(m_fieldGetOffset, nativeFault, field))
                                            : -1;
}

int MonoResolver::TypeKind(MonoType* type) const
{
    if (!m_typeGetType || !type) return 0;
    bool nativeFault = false;
    return SafeMonoCall(m_typeGetType, nativeFault, type);
}

bool MonoResolver::TypeIsByRef(MonoType* type) const
{
    if (!m_typeIsByRef || !type) return false;
    bool nativeFault = false;
    return SafeMonoCall(m_typeIsByRef, nativeFault, type) != 0;
}

MonoClass* MonoResolver::TypeClass(MonoType* type) const
{
    if (!m_classFromMonoType || !type) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_classFromMonoType, nativeFault, type);
}

MonoType* MonoResolver::ClassType(MonoClass* klass) const
{
    if (!m_classGetType || !klass) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_classGetType, nativeFault, klass);
}

MonoType* MonoResolver::EnumBaseType(MonoClass* klass) const
{
    if (!m_classEnumBaseType || !klass) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_classEnumBaseType, nativeFault, klass);
}
