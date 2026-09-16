// Lua 内部支持 C++ 异常。公开符号保持 C ABI，即使这个强制包含文件
// 位于 LUA_CORE/LUA_LIB 定义之前也要导出这些符号。
// Lua 使用 /EHs 编译：C ABI 调用可以抛出 C++ 异常，原生 SEH 必须交给桥接层处理。
// 不定义 LUA_USE_LONGJMP，普通 Lua 错误需要展开桥接层的 RAII 对象。
#pragma once
// 平台和 CRT 定义必须像 Lua 源码一样位于公开头文件之前。
#include "lprefix.h"
#define LUA_CORE
#include "lua.hpp"
#undef LUA_CORE
