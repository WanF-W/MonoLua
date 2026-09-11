// Shared Mono value classification; no Lua overload or IPC policy.
#pragma once
#include "mono_resolver.h"
#include "mono_metadata.h"

namespace mono_value
{
    using namespace mono_metadata;
    inline bool IsReferenceType(MonoResolver& resolver, MonoType* type, int kind)
    {
        if (kind == TYPE_STRING || kind == TYPE_CLASS || kind == TYPE_OBJECT || kind == TYPE_ARRAY ||
            kind == TYPE_SZARRAY)
            return true;
        if (kind != TYPE_GENERICINST) return false;
        MonoClass* klass = resolver.TypeClass(type);
        return klass && !resolver.ClassIsValueType(klass);
    }

    inline int StorageTypeKind(MonoResolver& resolver, MonoType* type)
    {
        const int kind = resolver.TypeKind(type);
        if (kind == TYPE_GENERICINST)
        {
            MonoClass* genericClass = resolver.TypeClass(type);
            return genericClass && resolver.ClassIsValueType(genericClass) ? TYPE_VALUETYPE : kind;
        }
        if (kind != TYPE_VALUETYPE) return kind;
        MonoClass* klass = resolver.TypeClass(type);
        if (!klass || !resolver.ClassIsEnum(klass)) return kind;
        return resolver.TypeKind(resolver.EnumBaseType(klass));
    }

    inline bool IsNullable(MonoResolver& resolver, MonoType* type)
    {
        if (resolver.TypeKind(type) != TYPE_GENERICINST) return false;
        MonoClass* klass = resolver.TypeClass(type);
        const char* name = resolver.ClassName(klass);
        const char* nameSpace = resolver.ClassNamespace(klass);
        return name && nameSpace && strcmp(name, "Nullable`1") == 0 && strcmp(nameSpace, "System") == 0;
    }

} // namespace mono_value
