/**
 * mono_resolver.cpp — Mono 模块与导出解析实现
 * 当前实现只接受正常独立加载并保留 Embedding API 导出的 Mono 环境。
 * 不扫描特征码，不从游戏 EXE 中搜索静态内嵌 Mono，也不加载额外 DLL。
 */
#include "mono_resolver.h"
#include "mono_metadata.h"
#include "mono_handle.h"

#include <sstream>

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

    // 模块名来自 Windows 宽字符 API。错误信息通过 UTF-8 管道发送给 Lune，
    // 因此不能用 wstring 迭代器直接构造 string；那既会产生 C4244，也会在
    // 文件名包含非 ASCII 字符时破坏文本。
    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty()) return {};

        const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                             nullptr, 0, nullptr, nullptr);
        if (size <= 0) return "<invalid module name>";

        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr);
        return result;
    }
} // namespace

MonoResolver& MonoResolver::Instance()
{
    static MonoResolver resolver;
    return resolver;
}

bool MonoResolver::Init()
{
    if (m_initialized) return true;
    m_lastError.clear();

    // Unity 不同年代常见的模块名不同。优先检查新版 bdwgc 名称，再兼容
    // 老版本 mono.dll。只使用 GetModuleHandleW，绝不主动加载另一套 Mono。
    static constexpr const wchar_t* candidates[] = {L"mono-2.0-bdwgc.dll", L"mono-2.0-sgen.dll", L"mono.dll"};
    for (const wchar_t* candidate : candidates)
    {
        m_module = GetModuleHandleW(candidate);
        if (m_module != nullptr)
        {
            m_moduleName = candidate;
            break;
        }
    }
    if (m_module == nullptr)
    {
        m_lastError = "Mono module not found; tried bdwgc, sgen, and legacy Mono DLL names";
        Shutdown();
        return false;
    }

    if (!ResolveExports())
    {
        std::ostringstream error;
        error << "required Mono exports missing from ";
        error << WideToUtf8(m_moduleName) << ": ";
        for (size_t index = 0; index < m_missingRequired.size(); ++index)
        {
            if (index != 0) error << ", ";
            error << m_missingRequired[index];
        }
        m_lastError = error.str();
        Shutdown();
        return false;
    }

    // Root Domain 只用于附加 MonoLua 自己创建的工作线程。对象创建、装箱
    // 和静态字段必须使用调用线程的当前 Domain，不能机械复用此指针。
    m_rootDomain = m_getRootDomain();
    m_initialized = m_rootDomain != nullptr;
    if (!m_initialized)
    {
        m_lastError = "mono_get_root_domain returned null";
        Shutdown();
    }
    return m_initialized;
}

bool MonoResolver::ResolveExports()
{
    m_missingOptional.clear();
    m_missingRequired.clear();
    m_resolvedCount = 0;
    m_exportCount = 0;

    // required 缺少任意一个都会使最小反射链路不成立；optional 缺失只
    // 关闭对应能力并记录到状态中，不能留下空函数指针继续执行。
    const auto required = [this](auto& target, const char* name) {
        ++m_exportCount;
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(GetProcAddress(m_module, name));
        if (target != nullptr)
        {
            ++m_resolvedCount;
        }
        else
        {
            m_missingRequired.emplace_back(name);
        }
        return target != nullptr;
    };
    const auto optional = [this](auto& target, const char* name) {
        ++m_exportCount;
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(GetProcAddress(m_module, name));
        if (target != nullptr)
            ++m_resolvedCount;
        else
            m_missingOptional.emplace_back(name);
    };

    // 下列导出覆盖反射、调用、对象、数组和值类型的完整公开能力。
    // 线程 attach/detach 必须成对可用；诊断和对象头深度校验按 optional 处理。
    bool ok = true;
    ok &= required(m_getRootDomain, "mono_get_root_domain");
    ok &= required(m_domainGet, "mono_domain_get");
    ok &= required(m_threadAttach, "mono_thread_attach");
    ok &= required(m_assemblyForeach, "mono_assembly_foreach");
    ok &= required(m_assemblyGetImage, "mono_assembly_get_image");
    ok &= required(m_imageGetName, "mono_image_get_name");
    ok &= required(m_classFromName, "mono_class_from_name");
    ok &= required(m_imageGetTableInfo, "mono_image_get_table_info");
    ok &= required(m_tableInfoGetRows, "mono_table_info_get_rows");
    ok &= required(m_classGet, "mono_class_get");
    ok &= required(m_classGetName, "mono_class_get_name");
    ok &= required(m_classGetNamespace, "mono_class_get_namespace");
    ok &= required(m_classGetParent, "mono_class_get_parent");
    ok &= required(m_classGetImage, "mono_class_get_image");
    ok &= required(m_classIsValueType, "mono_class_is_valuetype");
    ok &= required(m_classIsEnum, "mono_class_is_enum");
    optional(m_classInstanceSize, "mono_class_instance_size");
    ok &= required(m_classGetMethods, "mono_class_get_methods");
    ok &= required(m_classGetFields, "mono_class_get_fields");
    optional(m_classNumMethods, "mono_class_num_methods");
    optional(m_classNumFields, "mono_class_num_fields");
    ok &= required(m_classGetFieldFromName, "mono_class_get_field_from_name");
    ok &= required(m_methodGetName, "mono_method_get_name");
    ok &= required(m_methodGetClass, "mono_method_get_class");
    ok &= required(m_methodSignature, "mono_method_signature");
    ok &= required(m_methodGetFlags, "mono_method_get_flags");
    optional(m_methodIsGeneric, "mono_method_is_generic");
    optional(m_methodGetObject, "mono_method_get_object");
    optional(m_methodGetParamNames, "mono_method_get_param_names");
    optional(m_methodIsInflated, "mono_method_is_inflated");
    ok &= required(m_signatureGetReturnType, "mono_signature_get_return_type");
    ok &= required(m_signatureGetParams, "mono_signature_get_params");
    ok &= required(m_typeGetName, "mono_type_get_name");
    ok &= required(m_fieldGetName, "mono_field_get_name");
    ok &= required(m_fieldGetParent, "mono_field_get_parent");
    ok &= required(m_fieldGetType, "mono_field_get_type");
    optional(m_fieldGetOffset, "mono_field_get_offset");
    ok &= required(m_fieldGetFlags, "mono_field_get_flags");
    ok &= required(m_typeGetType, "mono_type_get_type");
    ok &= required(m_typeIsByRef, "mono_type_is_byref");
    ok &= required(m_classFromMonoType, "mono_class_from_mono_type");
    ok &= required(m_classGetType, "mono_class_get_type");
    ok &= required(m_typeGetObject, "mono_type_get_object");
    ok &= required(m_classEnumBaseType, "mono_class_enum_basetype");
    ok &= required(m_fieldGetValue, "mono_field_get_value");
    ok &= required(m_fieldSetValue, "mono_field_set_value");
    optional(m_fieldStaticGetValue, "mono_field_static_get_value");
    optional(m_fieldStaticSetValue, "mono_field_static_set_value");
    optional(m_runtimeClassInit, "mono_runtime_class_init");
    optional(m_stringNewLen, "mono_string_new_len");
    optional(m_stringChars, "mono_string_chars");
    optional(m_stringLength, "mono_string_length");
    ok &= required(m_runtimeInvoke, "mono_runtime_invoke");
    ok &= required(m_objectUnbox, "mono_object_unbox");
    optional(m_objectToString, "mono_object_to_string");
    optional(m_compileMethod, "mono_compile_method");
    optional(m_lookupInternalCall, "mono_lookup_internal_call");
    optional(m_arrayLength, "mono_array_length");
    optional(m_arrayAddrWithSize, "mono_array_addr_with_size");
    optional(m_objectNew, "mono_object_new");
    optional(m_arrayNew, "mono_array_new");
    optional(m_classGetElementClass, "mono_class_get_element_class");
    optional(m_classGetRank, "mono_class_get_rank");
    optional(m_arrayElementSize, "mono_array_element_size");
    optional(m_gcWBarrierSetArrayRef, "mono_gc_wbarrier_set_arrayref");
    optional(m_valueBox, "mono_value_box");
    optional(m_valueCopy, "mono_value_copy");
    optional(m_fieldGetValueObject, "mono_field_get_value_object");
    ok &= required(m_monoFree, "mono_free");
    ok &= required(m_objectGetClass, "mono_object_get_class");
    ok &= required(m_objectIsInst, "mono_object_isinst");
    ok &= required(m_gcHandleNew, "mono_gchandle_new");
    ok &= required(m_gcHandleGetTarget, "mono_gchandle_get_target");
    ok &= required(m_gcHandleFree, "mono_gchandle_free");
    ok &= required(m_threadDetach, "mono_thread_detach");
    ok &= required(m_objectGetVTable, "mono_object_get_vtable");
    optional(m_vtableClass, "mono_vtable_class");
    ok &= required(m_vtableDomain, "mono_vtable_domain");
    optional(m_classVTable, "mono_class_vtable");
    return ok;
}

void MonoResolver::Shutdown()
{
    // Resolver 从未增加目标模块引用计数，因此这里不能 FreeLibrary。
    // 所有 Mono 指针都是借用值，只需清空本地状态防止卸载后误用。
    m_module = nullptr;
    m_moduleName.clear();
    m_rootDomain = nullptr;
    m_getRootDomain = nullptr;
    m_domainGet = nullptr;
    m_threadAttach = nullptr;
    m_threadDetach = nullptr;
    m_assemblyForeach = nullptr;
    m_assemblyGetImage = nullptr;
    m_imageGetName = nullptr;
    m_classFromName = nullptr;
    m_imageGetTableInfo = nullptr;
    m_tableInfoGetRows = nullptr;
    m_classGet = nullptr;
    m_classGetName = nullptr;
    m_classGetNamespace = nullptr;
    m_classGetParent = nullptr;
    m_classGetImage = nullptr;
    m_classIsValueType = nullptr;
    m_classIsEnum = nullptr;
    m_classInstanceSize = nullptr;
    m_classGetMethods = nullptr;
    m_classGetFields = nullptr;
    m_classNumMethods = nullptr;
    m_classNumFields = nullptr;
    m_classGetFieldFromName = nullptr;
    m_methodGetName = nullptr;
    m_methodGetClass = nullptr;
    m_methodSignature = nullptr;
    m_methodGetFlags = nullptr;
    m_methodIsGeneric = nullptr;
    m_methodGetObject = nullptr;
    m_methodGetParamNames = nullptr;
    m_methodIsInflated = nullptr;
    m_signatureGetReturnType = nullptr;
    m_signatureGetParams = nullptr;
    m_typeGetName = nullptr;
    m_fieldGetName = nullptr;
    m_fieldGetParent = nullptr;
    m_fieldGetType = nullptr;
    m_fieldGetOffset = nullptr;
    m_fieldGetFlags = nullptr;
    m_typeGetType = nullptr;
    m_typeIsByRef = nullptr;
    m_classFromMonoType = nullptr;
    m_classGetType = nullptr;
    m_typeGetObject = nullptr;
    m_classEnumBaseType = nullptr;
    m_fieldGetValue = nullptr;
    m_fieldSetValue = nullptr;
    m_fieldStaticGetValue = nullptr;
    m_fieldStaticSetValue = nullptr;
    m_runtimeClassInit = nullptr;
    m_stringNewLen = nullptr;
    m_stringChars = nullptr;
    m_stringLength = nullptr;
    m_runtimeInvoke = nullptr;
    m_objectUnbox = nullptr;
    m_objectToString = nullptr;
    m_compileMethod = nullptr;
    m_lookupInternalCall = nullptr;
    m_arrayLength = nullptr;
    m_arrayAddrWithSize = nullptr;
    m_objectNew = nullptr;
    m_arrayNew = nullptr;
    m_classGetElementClass = nullptr;
    m_classGetRank = nullptr;
    m_arrayElementSize = nullptr;
    m_gcWBarrierSetArrayRef = nullptr;
    m_valueBox = nullptr;
    m_valueCopy = nullptr;
    m_fieldGetValueObject = nullptr;
    m_monoFree = nullptr;
    m_objectGetClass = nullptr;
    m_objectIsInst = nullptr;
    m_objectGetVTable = nullptr;
    m_vtableClass = nullptr;
    m_vtableDomain = nullptr;
    m_classVTable = nullptr;
    m_gcHandleNew = nullptr;
    m_gcHandleGetTarget = nullptr;
    m_gcHandleFree = nullptr;
    m_initialized = false;
}

std::string MonoResolver::ResolveStatus() const
{
    std::ostringstream output;
    output << m_resolvedCount << '/' << m_exportCount << " exports resolved";
    return output.str();
}
// 类型安全的薄包装
// 每个包装都先检查导出和输入指针。Resolver 不在这里制造业务错误文本，
// 由 Runtime 或 Lua Binding 根据调用语境决定返回 nil 还是抛出 Lua error。

MonoDomain* MonoResolver::CurrentDomain() const
{
    return m_domainGet ? m_domainGet() : nullptr;
}

MonoThread* MonoResolver::AttachThread() const
{
    return m_threadAttach && m_rootDomain ? m_threadAttach(m_rootDomain) : nullptr;
}

void MonoResolver::DetachThread(MonoThread* thread) const
{
    if (m_threadDetach && thread) m_threadDetach(thread);
}

void MonoResolver::EnumerateAssemblies(MonoAssemblyForeachCallback callback, void* data) const
{
    if (m_assemblyForeach && callback) m_assemblyForeach(callback, data);
}

MonoImage* MonoResolver::AssemblyImage(MonoAssembly* assembly) const
{
    return m_assemblyGetImage && assembly ? m_assemblyGetImage(assembly) : nullptr;
}

const char* MonoResolver::ImageName(MonoImage* image) const
{
    return m_imageGetName && image ? m_imageGetName(image) : nullptr;
}

MonoClass* MonoResolver::FindClass(MonoImage* image, const char* ns, const char* name) const
{
    return m_classFromName && image && ns && name ? m_classFromName(image, ns, name) : nullptr;
}

std::vector<MonoClass*> MonoResolver::EnumerateClasses(MonoImage* image) const
{
    std::vector<MonoClass*> classes;
    if (!image || !m_imageGetTableInfo || !m_tableInfoGetRows || !m_classGet) return classes;

    // ECMA-335 元数据表编号 2 是 TypeDef；Class token 的高字节 0x02，
    // 低 24 位是从 1 开始的行号。无法解析的特殊条目直接跳过。
    constexpr int MONO_TABLE_TYPEDEF = 2;
    constexpr uint32_t MONO_TOKEN_TYPE_DEF = 0x02000000;
    const MonoTableInfo* table = m_imageGetTableInfo(image, MONO_TABLE_TYPEDEF);
    const int rows = table ? m_tableInfoGetRows(table) : 0;
    if (rows <= 0) return classes;

    classes.reserve(static_cast<size_t>(rows));
    for (int row = 1; row <= rows; ++row)
    {
        MonoClass* klass = m_classGet(image, MONO_TOKEN_TYPE_DEF | static_cast<uint32_t>(row));
        if (klass) classes.push_back(klass);
    }
    return classes;
}

const char* MonoResolver::ClassName(MonoClass* klass) const
{
    return m_classGetName && klass ? m_classGetName(klass) : nullptr;
}

const char* MonoResolver::ClassNamespace(MonoClass* klass) const
{
    return m_classGetNamespace && klass ? m_classGetNamespace(klass) : nullptr;
}

MonoClass* MonoResolver::ClassParent(MonoClass* klass) const
{
    return m_classGetParent && klass ? m_classGetParent(klass) : nullptr;
}

MonoImage* MonoResolver::ClassImage(MonoClass* klass) const
{
    return m_classGetImage && klass ? m_classGetImage(klass) : nullptr;
}

bool MonoResolver::ClassIsValueType(MonoClass* klass) const
{
    return m_classIsValueType && klass && m_classIsValueType(klass) != 0;
}

bool MonoResolver::ClassIsEnum(MonoClass* klass) const
{
    return m_classIsEnum && klass && m_classIsEnum(klass) != 0;
}

int32_t MonoResolver::ClassInstanceSize(MonoClass* klass) const
{
    return m_classInstanceSize && klass ? m_classInstanceSize(klass) : 0;
}

std::vector<MonoMethod*> MonoResolver::EnumerateMethods(MonoClass* klass) const
{
    std::vector<MonoMethod*> methods;
    if (!klass || !m_classGetMethods) return methods;
    if (m_classNumMethods)
    {
        const int count = m_classNumMethods(klass);
        if (count > 0) methods.reserve(static_cast<size_t>(count));
    }
    void* iterator = nullptr;
    while (MonoMethod* method = m_classGetMethods(klass, &iterator))
    {
        if (!m_methodGetClass || m_methodGetClass(method) == klass) methods.push_back(method);
    }
    return methods;
}

std::vector<MonoClassField*> MonoResolver::EnumerateFields(MonoClass* klass) const
{
    std::vector<MonoClassField*> fields;
    if (!klass || !m_classGetFields) return fields;
    if (m_classNumFields)
    {
        const int count = m_classNumFields(klass);
        if (count > 0) fields.reserve(static_cast<size_t>(count));
    }
    void* iterator = nullptr;
    while (MonoClassField* field = m_classGetFields(klass, &iterator))
    {
        if (!m_fieldGetParent || m_fieldGetParent(field) == klass) fields.push_back(field);
    }
    return fields;
}

MonoClassField* MonoResolver::FindField(MonoClass* klass, const char* name) const
{
    MonoClassField* field =
        m_classGetFieldFromName && klass && name ? m_classGetFieldFromName(klass, name) : nullptr;
    // mono_class_get_field_from_name 在部分版本会沿父类查找；MonoLua 的
    // Class API 只允许当前声明类，防止同名隐藏字段产生不确定结果。
    return field && (!m_fieldGetParent || m_fieldGetParent(field) == klass) ? field : nullptr;
}

MonoClassField* MonoResolver::FindFieldInHierarchy(MonoClass* klass, const char* name) const
{
    for (MonoClass* current = klass; current; current = ClassParent(current))
        if (MonoClassField* field = FindField(current, name)) return field;
    return nullptr;
}

const char* MonoResolver::MethodName(MonoMethod* method) const
{
    return m_methodGetName && method ? m_methodGetName(method) : nullptr;
}

MonoClass* MonoResolver::MethodClass(MonoMethod* method) const
{
    return m_methodGetClass && method ? m_methodGetClass(method) : nullptr;
}

std::vector<std::string> MonoResolver::MethodParameterTypes(MonoMethod* method) const
{
    std::vector<std::string> types;
    MonoMethodSignature* signature = m_methodSignature && method ? m_methodSignature(method) : nullptr;
    if (!signature || !m_signatureGetParams || !m_typeGetName) return types;

    void* iterator = nullptr;
    while (MonoType* type = m_signatureGetParams(signature, &iterator))
    {
        char* rawName = m_typeGetName(type);
        types.emplace_back(rawName ? rawName : "<unknown>");
        if (rawName && m_monoFree) m_monoFree(rawName);
    }
    return types;
}

std::vector<MonoType*> MonoResolver::MethodParameters(MonoMethod* method) const
{
    std::vector<MonoType*> types;
    MonoMethodSignature* signature = m_methodSignature && method ? m_methodSignature(method) : nullptr;
    if (!signature || !m_signatureGetParams) return types;
    void* iterator = nullptr;
    while (MonoType* type = m_signatureGetParams(signature, &iterator))
        types.push_back(type);
    return types;
}

MonoType* MonoResolver::MethodReturnType(MonoMethod* method) const
{
    MonoMethodSignature* signature = m_methodSignature && method ? m_methodSignature(method) : nullptr;
    return signature && m_signatureGetReturnType ? m_signatureGetReturnType(signature) : nullptr;
}

uint32_t MonoResolver::MethodFlags(MonoMethod* method) const
{
    return m_methodGetFlags && method ? m_methodGetFlags(method, nullptr) : 0;
}

bool MonoResolver::MethodIsGeneric(MonoMethod* method) const
{
    if (!method) return false;
    return (m_methodIsGeneric && m_methodIsGeneric(method) != 0) ||
           (m_methodIsInflated && m_methodIsInflated(method) != 0);
}

MonoObject* MonoResolver::MethodObject(MonoDomain* domain, MonoMethod* method) const
{
    return m_methodGetObject && domain && method ? m_methodGetObject(domain, method, MethodClass(method))
                                                 : nullptr;
}

MonoObject* MonoResolver::Invoke(MonoMethod* method, void* object, void** parameters,
                                 MonoObject** exception) const
{
    bridge_lifecycle::ManagedCallScope callScope;
    MonoObject* result = m_runtimeInvoke && method ? m_runtimeInvoke(method, object, parameters, exception) : nullptr;
    // A nested detour can quarantine the VM while runtime_invoke is in flight.
    // Propagate before callers perform reflection, root allocation or Lua error handling.
    if (bridge_lifecycle::g_sessionFaulted.load())
        RaiseException(bridge_lifecycle::SESSION_FAULT_CODE, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return result;
}

void* MonoResolver::Unbox(MonoObject* object) const
{
    return m_objectUnbox && object ? m_objectUnbox(object) : nullptr;
}

std::string MonoResolver::ObjectString(MonoObject* object) const
{
    if (!object) return {};
    MonoObject* nestedException = nullptr;
    if (m_objectToString)
    {
        MonoString* text = m_objectToString(object, &nestedException);
        return !nestedException && text ? StringValue(text) : std::string{};
    }
    for (MonoClass* klass = ObjectClass(object); klass; klass = ClassParent(klass))
        for (MonoMethod* method : EnumerateMethods(klass))
            if (strcmp(MethodName(method), "ToString") == 0 && MethodParameters(method).empty())
            {
                MonoObject* text = Invoke(method, object, nullptr, &nestedException);
                return !nestedException && text ? StringValue(reinterpret_cast<MonoString*>(text))
                                                : std::string{};
            }
    return {};
}

void* MonoResolver::CompileMethod(MonoMethod* method) const
{
    return m_compileMethod && method ? m_compileMethod(method) : nullptr;
}

uint32_t MonoResolver::MethodImplementationFlags(MonoMethod* method) const
{
    uint32_t flags = 0;
    if (m_methodGetFlags && method) m_methodGetFlags(method, &flags);
    return flags;
}

void* MonoResolver::LookupInternalCall(MonoMethod* method) const
{
    return m_lookupInternalCall && method ? m_lookupInternalCall(method) : nullptr;
}

uintptr_t MonoResolver::ArrayLength(MonoArray* array) const
{
    return m_arrayLength && array ? m_arrayLength(array) : 0;
}

MonoObject* MonoResolver::ArrayElement(MonoArray* array, uintptr_t index) const
{
    if (!m_arrayAddrWithSize || !array) return nullptr;
    char* slot = m_arrayAddrWithSize(array, static_cast<int>(sizeof(void*)), index);
    return slot ? *reinterpret_cast<MonoObject**>(slot) : nullptr;
}

MonoObject* MonoResolver::NewObject(MonoDomain* domain, MonoClass* klass) const
{
    return m_objectNew && domain && klass ? m_objectNew(domain, klass) : nullptr;
}

MonoArray* MonoResolver::NewArray(MonoDomain* domain, MonoClass* elementClass, uintptr_t length) const
{
    return m_arrayNew && domain && elementClass ? m_arrayNew(domain, elementClass, length) : nullptr;
}

MonoClass* MonoResolver::ElementClass(MonoClass* arrayClass) const
{
    return m_classGetElementClass && arrayClass ? m_classGetElementClass(arrayClass) : nullptr;
}

int MonoResolver::ClassRank(MonoClass* klass) const
{
    return m_classGetRank && klass ? m_classGetRank(klass) : -1;
}

int MonoResolver::ArrayElementSize(MonoClass* arrayClass) const
{
    return m_arrayElementSize && arrayClass ? m_arrayElementSize(arrayClass) : 0;
}

void* MonoResolver::ArrayAddress(MonoArray* array, int elementSize, uintptr_t index) const
{
    return m_arrayAddrWithSize && array && elementSize > 0 ? m_arrayAddrWithSize(array, elementSize, index)
                                                           : nullptr;
}

bool MonoResolver::SetArrayReference(MonoArray* array, void* slot, MonoObject* value) const
{
    if (!m_gcWBarrierSetArrayRef || !array || !slot) return false;
    m_gcWBarrierSetArrayRef(array, slot, value);
    return true;
}

MonoObject* MonoResolver::Box(MonoDomain* domain, MonoClass* klass, void* value) const
{
    return m_valueBox && domain && klass && value ? m_valueBox(domain, klass, value) : nullptr;
}

bool MonoResolver::CopyValue(void* destination, void* source, MonoClass* klass) const
{
    if (!m_valueCopy || !destination || !source || !klass) return false;
    m_valueCopy(destination, source, klass);
    return true;
}

MonoObject* MonoResolver::FieldValueObject(MonoDomain* domain, MonoClassField* field,
                                           MonoObject* object) const
{
    return m_fieldGetValueObject && domain && field ? m_fieldGetValueObject(domain, field, object) : nullptr;
}

std::string MonoResolver::TypeDisplayName(MonoType* type) const
{
    char* raw = type && m_typeGetName ? m_typeGetName(type) : nullptr;
    std::string name = raw ? raw : "<unknown>";
    if (raw && m_monoFree) m_monoFree(raw);
    return name;
}

std::string MonoResolver::MethodSignature(MonoMethod* method) const
{
    if (!method) return {};
    MonoMethodSignature* signature = m_methodSignature ? m_methodSignature(method) : nullptr;
    MonoType* returnType =
        signature && m_signatureGetReturnType ? m_signatureGetReturnType(signature) : nullptr;
    char* rawReturn = returnType && m_typeGetName ? m_typeGetName(returnType) : nullptr;

    std::ostringstream output;
    uint32_t implementationFlags = 0;
    const uint32_t flags = m_methodGetFlags ? m_methodGetFlags(method, &implementationFlags) : 0;
    output << MemberAccess(flags) << ' ';
    if ((flags & mono_metadata::METHOD_ATTRIBUTE_STATIC) != 0) output << "static ";
    if ((flags & 0x0400) != 0)
        output << "abstract ";
    else if ((flags & 0x0040) != 0)
        output << "virtual ";
    if ((flags & 0x0020) != 0) output << "final ";
    output << (rawReturn ? rawReturn : "<unknown>") << ' ';
    if (rawReturn && m_monoFree) m_monoFree(rawReturn);
    MonoClass* klass = MethodClass(method);
    const char* nameSpace = ClassNamespace(klass);
    if (nameSpace && *nameSpace) output << nameSpace << '.';
    output << (ClassName(klass) ? ClassName(klass) : "<unknown>") << '.';
    output << (MethodName(method) ? MethodName(method) : "<unknown>") << '(';
    const auto parameters = MethodParameterTypes(method);
    std::vector<const char*> names(parameters.size(), nullptr);
    if (m_methodGetParamNames && !names.empty()) m_methodGetParamNames(method, names.data());
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
    return m_fieldGetName && field ? m_fieldGetName(field) : nullptr;
}

MonoClass* MonoResolver::FieldClass(MonoClassField* field) const
{
    return m_fieldGetParent && field ? m_fieldGetParent(field) : nullptr;
}

MonoType* MonoResolver::FieldType(MonoClassField* field) const
{
    return m_fieldGetType && field ? m_fieldGetType(field) : nullptr;
}

uint32_t MonoResolver::FieldFlags(MonoClassField* field) const
{
    return m_fieldGetFlags && field ? m_fieldGetFlags(field) : 0;
}

std::string MonoResolver::FieldSignature(MonoClassField* field) const
{
    if (!field) return {};
    MonoType* type = m_fieldGetType ? m_fieldGetType(field) : nullptr;
    char* rawType = type && m_typeGetName ? m_typeGetName(type) : nullptr;
    MonoClass* klass = FieldClass(field);

    std::ostringstream output;
    const uint32_t flags = m_fieldGetFlags ? m_fieldGetFlags(field) : 0;
    output << MemberAccess(flags) << ' ';
    if ((flags & mono_metadata::FIELD_ATTRIBUTE_STATIC) != 0) output << "static ";
    if ((flags & mono_metadata::FIELD_ATTRIBUTE_LITERAL) != 0)
        output << "const ";
    else if ((flags & mono_metadata::FIELD_ATTRIBUTE_INIT_ONLY) != 0)
        output << "readonly ";
    output << (rawType ? rawType : "<unknown>") << ' ';
    if (rawType && m_monoFree) m_monoFree(rawType);
    const char* nameSpace = ClassNamespace(klass);
    if (nameSpace && *nameSpace) output << nameSpace << '.';
    output << (ClassName(klass) ? ClassName(klass) : "<unknown>") << '.';
    output << (FieldName(field) ? FieldName(field) : "<unknown>");
    return output.str();
}

int32_t MonoResolver::FieldOffset(MonoClassField* field) const
{
    // 静态字段没有相对于实例对象的有效偏移，公开 API 按约定返回 nil。
    if (m_fieldGetFlags && field && (m_fieldGetFlags(field) & mono_metadata::FIELD_ATTRIBUTE_STATIC) != 0)
        return -1;
    return m_fieldGetOffset && field ? static_cast<int32_t>(m_fieldGetOffset(field)) : -1;
}

int MonoResolver::TypeKind(MonoType* type) const
{
    return m_typeGetType && type ? m_typeGetType(type) : 0;
}

bool MonoResolver::TypeIsByRef(MonoType* type) const
{
    return m_typeIsByRef && type && m_typeIsByRef(type) != 0;
}

MonoClass* MonoResolver::TypeClass(MonoType* type) const
{
    return m_classFromMonoType && type ? m_classFromMonoType(type) : nullptr;
}

MonoType* MonoResolver::ClassType(MonoClass* klass) const
{
    return m_classGetType && klass ? m_classGetType(klass) : nullptr;
}

MonoObject* MonoResolver::TypeObject(MonoDomain* domain, MonoType* type) const
{
    return m_typeGetObject && domain && type ? m_typeGetObject(domain, type) : nullptr;
}

MonoType* MonoResolver::EnumBaseType(MonoClass* klass) const
{
    return m_classEnumBaseType && klass ? m_classEnumBaseType(klass) : nullptr;
}

void MonoResolver::ReadField(MonoObject* object, MonoClassField* field, void* value) const
{
    if (m_fieldGetValue && object && field && value) m_fieldGetValue(object, field, value);
}

void MonoResolver::WriteField(MonoObject* object, MonoClassField* field, void* value) const
{
    // 引用字段写 nil 时 value 本身就是 nullptr；Mono 将其解释为清空引用，
    // 因此不能像读取输出缓冲区那样把空 value 当作无效参数过滤掉。
    if (m_fieldSetValue && object && field) m_fieldSetValue(object, field, value);
}

bool MonoResolver::ReadStaticField(MonoDomain* domain, MonoClassField* field, void* value) const
{
    if (!m_fieldStaticGetValue || !m_classVTable || !m_runtimeClassInit || !domain || !field || !value)
        return false;
    MonoVTable* vtable = m_classVTable(domain, FieldClass(field));
    if (!vtable) return false;
    m_runtimeClassInit(vtable);
    m_fieldStaticGetValue(vtable, field, value);
    return true;
}

bool MonoResolver::WriteStaticField(MonoDomain* domain, MonoClassField* field, void* value) const
{
    if (!m_fieldStaticSetValue || !m_classVTable || !m_runtimeClassInit || !domain || !field) return false;
    MonoVTable* vtable = m_classVTable(domain, FieldClass(field));
    if (!vtable) return false;
    m_runtimeClassInit(vtable);
    m_fieldStaticSetValue(vtable, field, value);
    return true;
}

MonoDomain* MonoResolver::ObjectDomain(MonoObject* object) const
{
    if (!m_objectGetVTable || !m_vtableDomain || !object) return nullptr;
    return m_vtableDomain(m_objectGetVTable(object));
}

MonoString* MonoResolver::NewString(MonoDomain* domain, const char* value, size_t length) const
{
    return m_stringNewLen && domain && value && length <= UINT32_MAX
               ? m_stringNewLen(domain, value, static_cast<uint32_t>(length))
               : nullptr;
}

std::string MonoResolver::StringValue(MonoString* value) const
{
    if (!CanReadStrings() || !value) return {};
    mono::ScopedGCHandle root(Instance(), CreateGCHandle(reinterpret_cast<MonoObject*>(value), true));
    if (!root.value) return {};
    value = reinterpret_cast<MonoString*>(root.Target());
    const int32_t length = m_stringLength(value);
    if (length <= 0) return {};
    const auto* chars = reinterpret_cast<const wchar_t*>(m_stringChars(value));
    if (!chars) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, chars, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, chars, length, result.data(), bytes, nullptr, nullptr);
    return result;
}

bool MonoResolver::IsValidObjectHeader(MonoObject* object) const
{
    if (!CanValidateObjectHeader() || !object) return false;

    // MonoObject 的首字段必须是该类型在所属 Domain 中唯一的 MonoVTable。
    // 类、方法等元数据虽然同样可读，但其首字段无法同时通过这组恒等关系，
    // 因而不能再像只调用 mono_object_get_class 时那样被误判为实例。
    __try
    {
        MonoVTable* vtable = m_objectGetVTable(object);
        if (!vtable) return false;
        MonoClass* klass = m_vtableClass(vtable);
        MonoDomain* domain = m_vtableDomain(vtable);
        return klass && domain && m_classVTable(domain, klass) == vtable;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool MonoResolver::ObjectIsInstanceOf(MonoObject* object, MonoClass* klass) const
{
    return m_objectIsInst && object && klass && m_objectIsInst(object, klass) != nullptr;
}

MonoClass* MonoResolver::ObjectClass(MonoObject* object) const
{
    if (!m_objectGetClass || !object) return nullptr;

    // wrap() 接受研究者提供的裸地址。即使 VirtualQuery 显示可读，对象头
    // 仍可能不是合法 MonoObject；SEH 在最小调用边界拦截访问异常，不能让
    // 一个错误地址终止整个 DLL 工作线程。
    __try
    {
        return m_objectGetClass(object);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

uint32_t MonoResolver::CreateGCHandle(MonoObject* object, bool pinned) const
{
    return m_gcHandleNew && object ? m_gcHandleNew(object, pinned ? TRUE : FALSE) : 0;
}

MonoObject* MonoResolver::GCHandleTarget(uint32_t handle) const
{
    return m_gcHandleGetTarget && handle ? m_gcHandleGetTarget(handle) : nullptr;
}

void MonoResolver::FreeGCHandle(uint32_t handle) const
{
    if (m_gcHandleFree && handle) m_gcHandleFree(handle);
}
