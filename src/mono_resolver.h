/**
 * ============================================================
 * mono_resolver.h — Mono 模块定位与导出解析层
 * ============================================================
 * 本模块只负责：
 * ·从候选 DLL 名称中定位目标进程已经加载的 Mono 模块
 * ·按 required/optional 分类解析 Embedding API
 * ·保存 Root Domain 和导出解析状态
 * ·提供薄的类型安全函数包装
 *
 * 本模块不缓存程序集、不选择重载、不处理 Lua 参数，也不拥有任何
 * Mono 对象。高层运行时语义统一放在 MonoRuntime 中。
 * ============================================================
 */
#pragma once

#include "common.h"
#include "mono_api.h"

class MonoResolver
{
public:
    static MonoResolver& Instance();
    bool Init();
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }
    HMODULE Module() const { return m_module; }
    MonoDomain* RootDomain() const { return m_rootDomain; }
    MonoDomain* CurrentDomain() const;
    const std::wstring& ModuleName() const { return m_moduleName; }
    std::string ResolveStatus() const;
    const std::string& LastError() const { return m_lastError; }

    MonoThread* CurrentThread() const;
    MonoThread* AttachThread() const;
    void DetachThread(MonoThread* thread) const;
    void EnumerateAssemblies(MonoAssemblyForeachCallback callback, void* userData) const;
    MonoImage* AssemblyImage(MonoAssembly* assembly) const;
    const char* ImageName(MonoImage* image) const;
    MonoClass* FindClass(MonoImage* image, const char* nameSpace, const char* name) const;
    std::vector<MonoClass*> EnumerateClasses(MonoImage* image) const;
    const char* ClassName(MonoClass* klass) const;
    const char* ClassNamespace(MonoClass* klass) const;
    MonoClass* ClassParent(MonoClass* klass) const;
    MonoImage* ClassImage(MonoClass* klass) const;
    bool ClassIsValueType(MonoClass* klass) const;
    bool ClassIsEnum(MonoClass* klass) const;
    int32_t ClassInstanceSize(MonoClass* klass) const;
    std::vector<MonoMethod*> EnumerateMethods(MonoClass* klass) const;
    std::vector<MonoClassField*> EnumerateFields(MonoClass* klass) const;
    MonoClassField* FindField(MonoClass* klass, const char* name) const;
    const char* MethodName(MonoMethod* method) const;
    MonoClass* MethodClass(MonoMethod* method) const;
    std::string MethodSignature(MonoMethod* method) const;
    std::vector<std::string> MethodParameterTypes(MonoMethod* method) const;
    std::vector<MonoType*> MethodParameters(MonoMethod* method) const;
    MonoType* MethodReturnType(MonoMethod* method) const;
    uint32_t MethodFlags(MonoMethod* method) const;
    bool MethodIsGeneric(MonoMethod* method) const;
    bool CanInspectGenericMethods() const
    {
        return m_methodIsGeneric && m_methodIsInflated;
    }
    MonoObject* Invoke(MonoMethod* method, MonoObject* object,
        void** parameters, MonoObject** exception) const;
    void* Unbox(MonoObject* object) const;
    std::string ObjectString(MonoObject* object) const;
    void* CompileMethod(MonoMethod* method) const;
    uintptr_t ArrayLength(MonoArray* array) const;
    MonoObject* ArrayElement(MonoArray* array, uintptr_t index) const;
    MonoObject* NewObject(MonoDomain* domain, MonoClass* klass) const;
    MonoArray* NewArray(MonoDomain* domain, MonoClass* elementClass, uintptr_t length) const;
    MonoClass* ElementClass(MonoClass* arrayClass) const;
    int ClassRank(MonoClass* klass) const;
    int ArrayElementSize(MonoClass* arrayClass) const;
    void* ArrayAddress(MonoArray* array, int elementSize, uintptr_t index) const;
    bool SetArrayReference(MonoArray* array, void* slot, MonoObject* value) const;
    MonoObject* Box(MonoDomain* domain, MonoClass* klass, void* value) const;
    bool CopyValue(void* destination, void* source, MonoClass* klass) const;
    MonoObject* FieldValueObject(
        MonoDomain* domain, MonoClassField* field, MonoObject* object) const;
    const char* FieldName(MonoClassField* field) const;
    MonoClass* FieldClass(MonoClassField* field) const;
    MonoType* FieldType(MonoClassField* field) const;
    uint32_t FieldFlags(MonoClassField* field) const;
    std::string FieldSignature(MonoClassField* field) const;
    int32_t FieldOffset(MonoClassField* field) const;
    int TypeKind(MonoType* type) const;
    bool TypeIsByRef(MonoType* type) const;
    MonoClass* TypeClass(MonoType* type) const;
    MonoType* ClassType(MonoClass* klass) const;
    MonoObject* TypeObject(MonoType* type) const;
    MonoType* EnumBaseType(MonoClass* klass) const;
    void ReadField(MonoObject* object, MonoClassField* field, void* value) const;
    void WriteField(MonoObject* object, MonoClassField* field, void* value) const;
    bool ReadStaticField(MonoDomain* domain, MonoClassField* field, void* value) const;
    bool WriteStaticField(MonoDomain* domain, MonoClassField* field, void* value) const;
    MonoDomain* ObjectDomain(MonoObject* object) const;
    MonoString* NewString(MonoDomain* domain, const char* value) const;
    std::string StringValue(MonoString* value) const;
    bool CanValidateObjectHeader() const
    {
        return m_objectGetVTable && m_vtableClass && m_vtableDomain && m_classVTable;
    }
    bool IsValidObjectHeader(MonoObject* object) const;
    bool ObjectIsInstanceOf(MonoObject* object, MonoClass* klass) const;
    MonoClass* ObjectClass(MonoObject* object) const;
    uint32_t CreateGCHandle(MonoObject* object, bool pinned = false) const;
    MonoObject* GCHandleTarget(uint32_t handle) const;
    void FreeGCHandle(uint32_t handle) const;

private:
    MonoResolver() = default;
    bool ResolveExports();

    // 动态导出函数签名集中定义，避免在实现文件中散落强制转换。
    using FnGetRootDomain = MonoDomain*(*)();
    using FnDomainGet = MonoDomain*(*)();
    using FnThreadCurrent = MonoThread*(*)();
    using FnThreadAttach = MonoThread*(*)(MonoDomain*);
    using FnThreadDetach = void(*)(MonoThread*);
    using FnAssemblyForeach = void(*)(MonoAssemblyForeachCallback, void*);
    using FnAssemblyGetImage = MonoImage*(*)(MonoAssembly*);
    using FnImageGetName = const char*(*)(MonoImage*);
    using FnClassFromName = MonoClass*(*)(MonoImage*, const char*, const char*);
    using FnImageGetTableInfo = const MonoTableInfo*(*)(MonoImage*, int);
    using FnTableInfoGetRows = int(*)(const MonoTableInfo*);
    using FnClassGet = MonoClass*(*)(MonoImage*, uint32_t);
    using FnClassGetName = const char*(*)(MonoClass*);
    using FnClassGetNamespace = const char*(*)(MonoClass*);
    using FnClassGetParent = MonoClass*(*)(MonoClass*);
    using FnClassGetImage = MonoImage*(*)(MonoClass*);
    using FnClassIsValueType = int(*)(MonoClass*);
    using FnClassIsEnum = int(*)(MonoClass*);
    using FnClassInstanceSize = int32_t(*)(MonoClass*);
    using FnClassGetMethods = MonoMethod*(*)(MonoClass*, void**);
    using FnClassGetFields = MonoClassField*(*)(MonoClass*, void**);
    using FnClassNumMethods = int(*)(MonoClass*);
    using FnClassNumFields = int(*)(MonoClass*);
    using FnClassGetFieldFromName = MonoClassField*(*)(MonoClass*, const char*);
    using FnMethodGetName = const char*(*)(MonoMethod*);
    using FnMethodGetClass = MonoClass*(*)(MonoMethod*);
    using FnMethodSignature = MonoMethodSignature*(*)(MonoMethod*);
    using FnMethodGetFlags = uint32_t(*)(MonoMethod*, uint32_t*);
    using FnMethodIsGeneric = int(*)(MonoMethod*);
    using FnSignatureGetReturnType = MonoType*(*)(MonoMethodSignature*);
    using FnSignatureGetParams = MonoType*(*)(MonoMethodSignature*, void**);
    using FnTypeGetName = char*(*)(MonoType*);
    using FnFieldGetName = const char*(*)(MonoClassField*);
    using FnFieldGetParent = MonoClass*(*)(MonoClassField*);
    using FnFieldGetType = MonoType*(*)(MonoClassField*);
    using FnFieldGetOffset = uint32_t(*)(MonoClassField*);
    using FnFieldGetFlags = uint32_t(*)(MonoClassField*);
    using FnTypeGetType = int(*)(MonoType*);
    using FnTypeIsByRef = int(*)(MonoType*);
    using FnClassFromMonoType = MonoClass*(*)(MonoType*);
    using FnClassGetType = MonoType*(*)(MonoClass*);
    using FnTypeGetObject = MonoObject*(*)(MonoType*);
    using FnClassEnumBaseType = MonoType*(*)(MonoClass*);
    using FnFieldGetValue = void(*)(MonoObject*, MonoClassField*, void*);
    using FnFieldSetValue = void(*)(MonoObject*, MonoClassField*, void*);
    using FnFieldStaticGetValue = void(*)(MonoVTable*, MonoClassField*, void*);
    using FnFieldStaticSetValue = void(*)(MonoVTable*, MonoClassField*, void*);
    using FnRuntimeClassInit = void(*)(MonoVTable*);
    using FnStringNew = MonoString*(*)(MonoDomain*, const char*);
    using FnStringToUtf8 = char*(*)(MonoString*);
    using FnRuntimeInvoke = MonoObject*(*)(MonoMethod*, void*, void**, MonoObject**);
    using FnObjectUnbox = void*(*)(MonoObject*);
    using FnObjectToString = MonoString*(*)(MonoObject*, MonoObject**);
    using FnCompileMethod = void*(*)(MonoMethod*);
    using FnArrayLength = uintptr_t(*)(MonoArray*);
    using FnArrayAddrWithSize = char*(*)(MonoArray*, int, uintptr_t);
    using FnObjectNew = MonoObject*(*)(MonoDomain*, MonoClass*);
    using FnArrayNew = MonoArray*(*)(MonoDomain*, MonoClass*, uintptr_t);
    using FnClassGetElementClass = MonoClass*(*)(MonoClass*);
    using FnClassGetRank = int(*)(MonoClass*);
    using FnArrayElementSize = int(*)(MonoClass*);
    using FnGCWBarrierSetArrayRef = void(*)(MonoArray*, void*, MonoObject*);
    using FnValueBox = MonoObject*(*)(MonoDomain*, MonoClass*, void*);
    using FnValueCopy = void(*)(void*, void*, MonoClass*);
    using FnFieldGetValueObject = MonoObject*(*)(MonoDomain*, MonoClassField*, MonoObject*);
    using FnMonoFree = void(*)(void*);
    using FnObjectGetClass = MonoClass*(*)(MonoObject*);
    using FnObjectIsInst = MonoObject*(*)(MonoObject*, MonoClass*);
    using FnObjectGetVTable = MonoVTable*(*)(MonoObject*);
    using FnVTableClass = MonoClass*(*)(MonoVTable*);
    using FnVTableDomain = MonoDomain*(*)(MonoVTable*);
    using FnClassVTable = MonoVTable*(*)(MonoDomain*, MonoClass*);
    using FnGCHandleNew = uint32_t(*)(MonoObject*, int);
    using FnGCHandleGetTarget = MonoObject*(*)(uint32_t);
    using FnGCHandleFree = void(*)(uint32_t);

    // 模块与 Root Domain 都由目标游戏拥有，Resolver 只保存借用指针。
    HMODULE m_module = nullptr;
    std::wstring m_moduleName;
    MonoDomain* m_rootDomain = nullptr;
    FnGetRootDomain m_getRootDomain = nullptr;
    FnDomainGet m_domainGet = nullptr;
    FnThreadCurrent m_threadCurrent = nullptr;
    FnThreadAttach m_threadAttach = nullptr;
    FnThreadDetach m_threadDetach = nullptr;
    FnAssemblyForeach m_assemblyForeach = nullptr;
    FnAssemblyGetImage m_assemblyGetImage = nullptr;
    FnImageGetName m_imageGetName = nullptr;
    FnClassFromName m_classFromName = nullptr;
    FnImageGetTableInfo m_imageGetTableInfo = nullptr;
    FnTableInfoGetRows m_tableInfoGetRows = nullptr;
    FnClassGet m_classGet = nullptr;
    FnClassGetName m_classGetName = nullptr;
    FnClassGetNamespace m_classGetNamespace = nullptr;
    FnClassGetParent m_classGetParent = nullptr;
    FnClassGetImage m_classGetImage = nullptr;
    FnClassIsValueType m_classIsValueType = nullptr;
    FnClassIsEnum m_classIsEnum = nullptr;
    FnClassInstanceSize m_classInstanceSize = nullptr;
    FnClassGetMethods m_classGetMethods = nullptr;
    FnClassGetFields m_classGetFields = nullptr;
    FnClassNumMethods m_classNumMethods = nullptr;
    FnClassNumFields m_classNumFields = nullptr;
    FnClassGetFieldFromName m_classGetFieldFromName = nullptr;
    FnMethodGetName m_methodGetName = nullptr;
    FnMethodGetClass m_methodGetClass = nullptr;
    FnMethodSignature m_methodSignature = nullptr;
    FnMethodGetFlags m_methodGetFlags = nullptr;
    FnMethodIsGeneric m_methodIsGeneric = nullptr;
    FnMethodIsGeneric m_methodIsInflated = nullptr;
    FnSignatureGetReturnType m_signatureGetReturnType = nullptr;
    FnSignatureGetParams m_signatureGetParams = nullptr;
    FnTypeGetName m_typeGetName = nullptr;
    FnFieldGetName m_fieldGetName = nullptr;
    FnFieldGetParent m_fieldGetParent = nullptr;
    FnFieldGetType m_fieldGetType = nullptr;
    FnFieldGetOffset m_fieldGetOffset = nullptr;
    FnFieldGetFlags m_fieldGetFlags = nullptr;
    FnTypeGetType m_typeGetType = nullptr;
    FnTypeIsByRef m_typeIsByRef = nullptr;
    FnClassFromMonoType m_classFromMonoType = nullptr;
    FnClassGetType m_classGetType = nullptr;
    FnTypeGetObject m_typeGetObject = nullptr;
    FnClassEnumBaseType m_classEnumBaseType = nullptr;
    FnFieldGetValue m_fieldGetValue = nullptr;
    FnFieldSetValue m_fieldSetValue = nullptr;
    FnFieldStaticGetValue m_fieldStaticGetValue = nullptr;
    FnFieldStaticSetValue m_fieldStaticSetValue = nullptr;
    FnRuntimeClassInit m_runtimeClassInit = nullptr;
    FnStringNew m_stringNew = nullptr;
    FnStringToUtf8 m_stringToUtf8 = nullptr;
    FnRuntimeInvoke m_runtimeInvoke = nullptr;
    FnObjectUnbox m_objectUnbox = nullptr;
    FnObjectToString m_objectToString = nullptr;
    FnCompileMethod m_compileMethod = nullptr;
    FnArrayLength m_arrayLength = nullptr;
    FnArrayAddrWithSize m_arrayAddrWithSize = nullptr;
    FnObjectNew m_objectNew = nullptr;
    FnArrayNew m_arrayNew = nullptr;
    FnClassGetElementClass m_classGetElementClass = nullptr;
    FnClassGetRank m_classGetRank = nullptr;
    FnArrayElementSize m_arrayElementSize = nullptr;
    FnGCWBarrierSetArrayRef m_gcWBarrierSetArrayRef = nullptr;
    FnValueBox m_valueBox = nullptr;
    FnValueCopy m_valueCopy = nullptr;
    FnFieldGetValueObject m_fieldGetValueObject = nullptr;
    FnMonoFree m_monoFree = nullptr;
    FnObjectGetClass m_objectGetClass = nullptr;
    FnObjectIsInst m_objectIsInst = nullptr;
    FnObjectGetVTable m_objectGetVTable = nullptr;
    FnVTableClass m_vtableClass = nullptr;
    FnVTableDomain m_vtableDomain = nullptr;
    FnClassVTable m_classVTable = nullptr;
    FnGCHandleNew m_gcHandleNew = nullptr;
    FnGCHandleGetTarget m_gcHandleGetTarget = nullptr;
    FnGCHandleFree m_gcHandleFree = nullptr;
    // optional 缺失不会阻止初始化，但必须出现在状态文本中。
    std::vector<std::string> m_missingOptional;
    std::vector<std::string> m_missingRequired;
    std::string m_lastError;
    int m_resolvedCount = 0;
    int m_exportCount = 0;
    bool m_initialized = false;
};
