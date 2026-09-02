/**
 * ============================================================
 * version.h — MonoLua 产品与协议版本唯一来源
 * ============================================================
 * C++ 源码、Windows 版本资源和 MLune 握手协议统一使用这里的常量。
 * 发布新版本时，MonoLua 与 MLune 的同名文件必须同步修改。
 *
 * RC_INVOKED 分支专供 Windows Resource Compiler。资源语法只能使用宏；
 * 普通 C++ 分支使用 inline constexpr，避免把常量错误实现为预处理宏。
 * ============================================================
 */
#pragma once

#ifdef RC_INVOKED
    #define MONOLUA_VERSION_NUMERIC 1,0,0,0
    #define MONOLUA_VERSION_STRING "1.0.0"
#else
inline constexpr int MONOLUA_VERSION_MAJOR = 1;
inline constexpr int MONOLUA_VERSION_MINOR = 0;
inline constexpr int MONOLUA_VERSION_PATCH = 0;
inline constexpr const char* MONOLUA_VERSION_STRING = "1.0.0";
inline constexpr const wchar_t* MONOLUA_VERSION_WSTRING = L"1.0.0";
inline constexpr const char* MONOLUA_PROTOCOL_VERSION = "MonoLua/1.0.0";
#endif
