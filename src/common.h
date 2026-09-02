/**
 * ============================================================
 * common.h — MonoLua 公共基础依赖
 * ============================================================
 * 只放置多个核心模块共同需要的 Windows/标准库声明和通用宏。
 * Mono API 类型集中在 mono_api.h，避免本文件变成运行时声明集合。
 * ============================================================
 */
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define MONOLUA_UNUSED(value) (void)(value)
