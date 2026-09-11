// Lua supports C++ exceptions internally. Keep its public symbols in the C ABI
// and export them even though this forced include precedes LUA_CORE/LUA_LIB.
// Compile Lua with /EHs: C ABI calls may throw, while native SEH must escape Lua's catch-all.
// No LUA_USE_LONGJMP: bridge RAII must unwind on ordinary Lua errors.
#pragma once
// Platform/CRT definitions must precede the public headers, just as in Lua's sources.
#include "lprefix.h"
#define LUA_CORE
#include "lua.hpp"
#undef LUA_CORE
