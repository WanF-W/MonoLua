/**
 * mono_metadata.h — Mono 元数据常量
 * MonoLua 不直接依赖目标游戏附带的 Mono 头文件，但仍需要识别少量
 * 稳定的 ECMA-335 类型和成员标志。统一放在这里，避免不同 Binding
 * 文件各自复制魔数，也避免把这些常量误放进业务模块。
 */
#pragma once

#include <cstdint>
#include <limits>

namespace mono_metadata
{
    constexpr uint32_t METHOD_ATTRIBUTE_STATIC = 0x0010;

    constexpr uint32_t FIELD_ATTRIBUTE_STATIC = 0x0010;
    constexpr uint32_t FIELD_ATTRIBUTE_INIT_ONLY = 0x0020;
    constexpr uint32_t FIELD_ATTRIBUTE_LITERAL = 0x0040;

    enum TypeKind : int
    {
        TYPE_VOID = 0x01,
        TYPE_BOOLEAN = 0x02,
        TYPE_CHAR = 0x03,
        TYPE_I1 = 0x04,
        TYPE_U1 = 0x05,
        TYPE_I2 = 0x06,
        TYPE_U2 = 0x07,
        TYPE_I4 = 0x08,
        TYPE_U4 = 0x09,
        TYPE_I8 = 0x0A,
        TYPE_U8 = 0x0B,
        TYPE_R4 = 0x0C,
        TYPE_R8 = 0x0D,
        TYPE_STRING = 0x0E,
        TYPE_BYREF = 0x10,
        TYPE_VALUETYPE = 0x11,
        TYPE_CLASS = 0x12,
        TYPE_ARRAY = 0x14,
        TYPE_GENERICINST = 0x15,
        TYPE_I = 0x18,
        TYPE_U = 0x19,
        TYPE_OBJECT = 0x1C,
        TYPE_SZARRAY = 0x1D
    };

    // Lua 使用有符号 64 位 integer；所有需要窄化的 Mono 整数都在进入
    // 运行时前经过这里的范围检查，避免字段、数组和 Hook 返回值各自实现
    // 一套略有差异的截断规则。
    inline bool IntegerFits(int kind, int64_t value)
    {
        switch (kind)
        {
        case TYPE_CHAR:
        case TYPE_U2:
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
            return value >= 0 && static_cast<uint64_t>(value) <= (std::numeric_limits<uint32_t>::max)();
        case TYPE_U8:
        case TYPE_U:
            // Lua's signed integer carries the complete unsigned 64-bit bit pattern.
            return true;
        default:
            return true;
        }
    }
} // namespace mono_metadata
