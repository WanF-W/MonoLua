#pragma once
#include "mono_feature_fault.h"
#include "mono_api.h"
namespace mono_value_memory
{
    template <typename Value>
    bool ReadValueSafely(const void* address, Value& value)
    {
        MonoFeatureFault fault;
        fault.stage = "value read";
        fault.catchAllMemoryAccess = true;
        __try
        {
            memcpy(&value, address, sizeof(Value));
            return true;
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (fault.code)
        {
            bridge_lifecycle::g_nativeCallFaulted = true;
            bridge_lifecycle::g_nativeCallFaultCode = fault.code;
        }
        return false;
    }

    inline bool WriteValueSafely(void* address, const void* value, size_t size)
    {
        MonoFeatureFault fault;
        fault.stage = "value write";
        fault.catchAllMemoryAccess = true;
        __try
        {
            memcpy(address, value, size);
            return true;
        }
        __except (fault.Filter(GetExceptionInformation()))
        {
        }
        if (fault.code)
        {
            bridge_lifecycle::g_nativeCallFaulted = true;
            bridge_lifecycle::g_nativeCallFaultCode = fault.code;
        }
        return false;
    }

    union FieldStorage {
        int8_t i1;
        uint8_t u1;
        int16_t i2;
        uint16_t u2;
        int32_t i4;
        uint32_t u4;
        int64_t i8;
        uint64_t u8;
        float r4;
        double r8;
        intptr_t nativeInt;
        uintptr_t nativeUInt;
        MonoObject* object;
    };

}
