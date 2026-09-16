// Runtime discovery and export binding only.
#include "mono_resolver.h"
#include "mono_metadata.h"
#include "mono_handle.h"
#include "mono_native_call.h"
#include <sstream>
using namespace mono_native;

namespace
{
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
}

MonoResolver& MonoResolver::Instance()
{
    static MonoResolver resolver;
    return resolver;
}

bool MonoResolver::Init()
{
    if (m_initialized) return true;
    bridge_lifecycle::ClearNativeCallFault();
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
    bool nativeFault = false;
    m_rootDomain = SafeMonoCall(m_getRootDomain, nativeFault);
    if (nativeFault)
    {
        m_lastError = "mono_get_root_domain raised an access violation";
        Shutdown();
        bridge_lifecycle::ClearNativeCallFault();
        return false;
    }
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
    optional(m_methodGetObject, "mono_method_get_object");
    optional(m_methodGetParamNames, "mono_method_get_param_names");
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
    // Never mix handle families: Unity's v2 handles can be addresses above 4 GB.
    const bool hasHandleV2 = GetProcAddress(m_module, "mono_gchandle_new_v2") &&
        GetProcAddress(m_module, "mono_gchandle_get_target_v2") &&
        GetProcAddress(m_module, "mono_gchandle_free_v2");
    if (hasHandleV2)
    {
        ok &= required(m_gcHandleNewV2, "mono_gchandle_new_v2");
        ok &= required(m_gcHandleGetTargetV2, "mono_gchandle_get_target_v2");
        ok &= required(m_gcHandleFreeV2, "mono_gchandle_free_v2");
    }
    else
    {
        ok &= required(m_gcHandleNew, "mono_gchandle_new");
        ok &= required(m_gcHandleGetTarget, "mono_gchandle_get_target");
        ok &= required(m_gcHandleFree, "mono_gchandle_free");
    }
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
    m_methodGetObject = nullptr;
    m_methodGetParamNames = nullptr;
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
    m_gcHandleNewV2 = nullptr;
    m_gcHandleGetTargetV2 = nullptr;
    m_gcHandleFreeV2 = nullptr;
    m_initialized = false;
}

std::string MonoResolver::ResolveStatus() const
{
    std::ostringstream output;
    output << m_resolvedCount << '/' << m_exportCount << " exports resolved";
    output << "; GCHandle: " << (m_gcHandleNewV2 ? "v2 (pointer-sized)" : "legacy (32-bit)");
    return output.str();
}
// 类型安全的薄包装
// 每个包装都先检查导出和输入指针。Resolver 不在这里制造业务错误文本，
// 由 Runtime 或 Lua Binding 根据调用语境决定返回 nil 还是抛出 Lua error。
