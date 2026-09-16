// Runtime execution, fields, objects, arrays and GCHandle ownership.
#include "mono_resolver.h"
#include "mono_metadata.h"
#include "mono_handle.h"
#include "mono_native_call.h"
#include <sstream>
using namespace mono_native;

namespace
{
    int SafeWideCharToMultiByte(const wchar_t* chars, int length, char* output, int outputLength) noexcept
    {
        __try
        {
            return WideCharToMultiByte(CP_UTF8, 0, chars, length, output, outputLength, nullptr, nullptr);
        }
        __except (AccessFaultFilter(GetExceptionInformation()))
        {
            bridge_lifecycle::g_nativeCallFaulted = true;
            bridge_lifecycle::g_nativeCallFaultCode = GetExceptionCode();
            return 0;
        }
    }

}

MonoDomain* MonoResolver::CurrentDomain() const
{
    if (!m_domainGet) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_domainGet, nativeFault);
}

MonoThread* MonoResolver::AttachThread() const
{
    if (!m_threadAttach || !m_rootDomain) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_threadAttach, nativeFault, m_rootDomain);
}

void MonoResolver::DetachThread(MonoThread* thread) const
{
    if (m_threadDetach && thread) SafeMonoCallVoid(m_threadDetach, thread);
}

MonoObject* MonoResolver::MethodObject(MonoDomain* domain, MonoMethod* method) const
{
    if (!m_methodGetObject || !domain || !method) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_methodGetObject, nativeFault, domain, method, MethodClass(method));
}

MonoObject* MonoResolver::Invoke(MonoMethod* method, void* object, void** parameters,
                                 MonoObject** exception) const
{
    bridge_lifecycle::ManagedCallScope callScope;
    if (!m_runtimeInvoke || !method) return nullptr;
    bool nativeFault = false;
    MonoObject* result = SafeMonoCall(m_runtimeInvoke, nativeFault, method, object, parameters, exception);
    // runtime_invoke 期间可能有嵌套 detour 隔离 VM；在调用方继续反射、
    // 创建根或处理 Lua 错误前先把状态传递出去。
    if (bridge_lifecycle::g_sessionFaulted.load())
        RaiseException(bridge_lifecycle::SESSION_FAULT_CODE, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return result;
}

void* MonoResolver::Unbox(MonoObject* object) const
{
    if (!m_objectUnbox || !object) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_objectUnbox, nativeFault, object);
}

std::string MonoResolver::ObjectString(MonoObject* object) const
{
    if (!object) return {};
    MonoObject* nestedException = nullptr;
    if (m_objectToString)
    {
        bool nativeFault = false;
        MonoString* text = SafeMonoCall(m_objectToString, nativeFault, object, &nestedException);
        return !nestedException && text ? StringValue(text) : std::string{};
    }
    for (MonoClass* klass = ObjectClass(object); klass; klass = ClassParent(klass))
        for (MonoMethod* method : EnumerateMethods(klass))
        {
            const char* methodName = MethodName(method);
            if (!methodName || strcmp(methodName, "ToString") != 0 || !MethodParameters(method).empty())
                continue;
            {
                MonoObject* text = Invoke(method, object, nullptr, &nestedException);
                return !nestedException && text ? StringValue(reinterpret_cast<MonoString*>(text))
                                                : std::string{};
            }
        }
    return {};
}

void* MonoResolver::CompileMethod(MonoMethod* method) const
{
    if (!m_compileMethod || !method) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_compileMethod, nativeFault, method);
}

void* MonoResolver::LookupInternalCall(MonoMethod* method) const
{
    if (!m_lookupInternalCall || !method) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_lookupInternalCall, nativeFault, method);
}

uintptr_t MonoResolver::ArrayLength(MonoArray* array) const
{
    if (!m_arrayLength || !array) return 0;
    bool nativeFault = false;
    return SafeMonoCall(m_arrayLength, nativeFault, array);
}

MonoObject* MonoResolver::ArrayElement(MonoArray* array, uintptr_t index) const
{
    if (!m_arrayAddrWithSize || !array) return nullptr;
    bool nativeFault = false;
    char* slot = SafeMonoCall(m_arrayAddrWithSize, nativeFault, array, static_cast<int>(sizeof(void*)), index);
    if (nativeFault) return nullptr;
    if (!slot) return nullptr;
    MonoObject* result = nullptr;
    __try
    {
        result = *reinterpret_cast<MonoObject**>(slot);
    }
    __except (AccessFaultFilter(GetExceptionInformation()))
    {
        bridge_lifecycle::g_nativeCallFaulted = true;
        bridge_lifecycle::g_nativeCallFaultCode = GetExceptionCode();
    }
    return result;
}

MonoObject* MonoResolver::NewObject(MonoDomain* domain, MonoClass* klass) const
{
    if (!m_objectNew || !domain || !klass) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_objectNew, nativeFault, domain, klass);
}

MonoArray* MonoResolver::NewArray(MonoDomain* domain, MonoClass* elementClass, uintptr_t length) const
{
    if (!m_arrayNew || !domain || !elementClass) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_arrayNew, nativeFault, domain, elementClass, length);
}

void* MonoResolver::ArrayAddress(MonoArray* array, int elementSize, uintptr_t index) const
{
    if (!m_arrayAddrWithSize || !array || elementSize <= 0) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_arrayAddrWithSize, nativeFault, array, elementSize, index);
}

bool MonoResolver::SetArrayReference(MonoArray* array, void* slot, MonoObject* value) const
{
    if (!m_gcWBarrierSetArrayRef || !array || !slot) return false;
    return SafeMonoCallVoid(m_gcWBarrierSetArrayRef, array, slot, value);
}

MonoObject* MonoResolver::Box(MonoDomain* domain, MonoClass* klass, void* value) const
{
    if (!m_valueBox || !domain || !klass || !value) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_valueBox, nativeFault, domain, klass, value);
}

bool MonoResolver::CopyValue(void* destination, void* source, MonoClass* klass) const
{
    if (!m_valueCopy || !destination || !source || !klass) return false;
    return SafeMonoCallVoid(m_valueCopy, destination, source, klass);
}

MonoObject* MonoResolver::FieldValueObject(MonoDomain* domain, MonoClassField* field,
                                           MonoObject* object) const
{
    if (!m_fieldGetValueObject || !domain || !field) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_fieldGetValueObject, nativeFault, domain, field, object);
}

MonoObject* MonoResolver::TypeObject(MonoDomain* domain, MonoType* type) const
{
    if (!m_typeGetObject || !domain || !type) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_typeGetObject, nativeFault, domain, type);
}

bool MonoResolver::ReadField(MonoObject* object, MonoClassField* field, void* value) const
{
    return m_fieldGetValue && object && field && value &&
        SafeMonoCallVoid(m_fieldGetValue, object, field, value);
}

bool MonoResolver::WriteField(MonoObject* object, MonoClassField* field, void* value) const
{
    // 引用字段写 nil 时 value 本身就是 nullptr；Mono 将其解释为清空引用，
    // 因此不能像读取输出缓冲区那样把空 value 当作无效参数过滤掉。
    return m_fieldSetValue && object && field && SafeMonoCallVoid(m_fieldSetValue, object, field, value);
}

bool MonoResolver::ReadStaticField(MonoDomain* domain, MonoClassField* field, void* value) const
{
    if (!m_fieldStaticGetValue || !m_classVTable || !m_runtimeClassInit || !domain || !field || !value)
        return false;
    bool nativeFault = false;
    MonoVTable* vtable = SafeMonoCall(m_classVTable, nativeFault, domain, FieldClass(field));
    if (!vtable) return false;
    if (!SafeMonoCallVoid(m_runtimeClassInit, vtable)) return false;
    return SafeMonoCallVoid(m_fieldStaticGetValue, vtable, field, value);
}

bool MonoResolver::WriteStaticField(MonoDomain* domain, MonoClassField* field, void* value) const
{
    if (!m_fieldStaticSetValue || !m_classVTable || !m_runtimeClassInit || !domain || !field) return false;
    bool nativeFault = false;
    MonoVTable* vtable = SafeMonoCall(m_classVTable, nativeFault, domain, FieldClass(field));
    if (!vtable) return false;
    if (!SafeMonoCallVoid(m_runtimeClassInit, vtable)) return false;
    return SafeMonoCallVoid(m_fieldStaticSetValue, vtable, field, value);
}

MonoDomain* MonoResolver::ObjectDomain(MonoObject* object) const
{
    if (!m_objectGetVTable || !m_vtableDomain || !object) return nullptr;
    bool nativeFault = false;
    MonoVTable* vtable = SafeMonoCall(m_objectGetVTable, nativeFault, object);
    return vtable && !nativeFault ? SafeMonoCall(m_vtableDomain, nativeFault, vtable) : nullptr;
}

MonoString* MonoResolver::NewString(MonoDomain* domain, const char* value, size_t length) const
{
    if (!m_stringNewLen || !domain || !value || length > UINT32_MAX) return nullptr;
    bool nativeFault = false;
    return SafeMonoCall(m_stringNewLen, nativeFault, domain, value, static_cast<uint32_t>(length));
}

std::string MonoResolver::StringValue(MonoString* value) const
{
    if (!CanReadStrings() || !value) return {};
    mono::ScopedGCHandle root(Instance(), CreateGCHandle(reinterpret_cast<MonoObject*>(value), true));
    if (!root.value) return {};
    value = reinterpret_cast<MonoString*>(root.Target());
    bool nativeFault = false;
    const int32_t length = SafeMonoCall(m_stringLength, nativeFault, value);
    if (length <= 0) return {};
    const auto* chars = reinterpret_cast<const wchar_t*>(SafeMonoCall(m_stringChars, nativeFault, value));
    if (!chars) return {};
    if (nativeFault) return {};
    const int bytes = SafeWideCharToMultiByte(chars, length, nullptr, 0);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (SafeWideCharToMultiByte(chars, length, result.data(), bytes) <= 0) return {};
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
    if (!m_objectIsInst || !object || !klass) return false;
    bool nativeFault = false;
    return SafeMonoCall(m_objectIsInst, nativeFault, object, klass) != nullptr;
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

MonoGCHandle MonoResolver::CreateGCHandle(MonoObject* object, bool pinned) const
{
    if (!object) return 0;
    bool nativeFault = false;
    if (m_gcHandleNewV2) return SafeMonoCall(m_gcHandleNewV2, nativeFault, object, pinned ? TRUE : FALSE);
    return m_gcHandleNew ? SafeMonoCall(m_gcHandleNew, nativeFault, object, pinned ? TRUE : FALSE) : 0;
}

MonoObject* MonoResolver::GCHandleTarget(MonoGCHandle handle) const
{
    if (!handle) return nullptr;
    bool nativeFault = false;
    if (m_gcHandleGetTargetV2) return SafeMonoCall(m_gcHandleGetTargetV2, nativeFault, handle);
    if (!m_gcHandleGetTarget || handle > UINT32_MAX) return nullptr;
    return SafeMonoCall(m_gcHandleGetTarget, nativeFault, static_cast<uint32_t>(handle));
}

void MonoResolver::FreeGCHandle(MonoGCHandle handle) const
{
    if (!handle) return;
    if (m_gcHandleFreeV2) SafeMonoCallVoid(m_gcHandleFreeV2, handle);
    else if (m_gcHandleFree && handle <= UINT32_MAX)
        SafeMonoCallVoid(m_gcHandleFree, static_cast<uint32_t>(handle));
}
