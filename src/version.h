// Release identity; Lune's MONO_PROFILE must use the same protocol string.
#pragma once
#define MONOLUA_VERSION_MAJOR 2
#define MONOLUA_VERSION_MINOR 0
#define MONOLUA_VERSION_PATCH 2
#define MONOLUA_STRING_IMPL(value) #value
#define MONOLUA_STRING(value) MONOLUA_STRING_IMPL(value)
#define MONOLUA_VERSION_NUMERIC MONOLUA_VERSION_MAJOR, MONOLUA_VERSION_MINOR, MONOLUA_VERSION_PATCH, 0
#define MONOLUA_VERSION_STRING                                                                               \
    MONOLUA_STRING(MONOLUA_VERSION_MAJOR)                                                                    \
    "." MONOLUA_STRING(MONOLUA_VERSION_MINOR) "." MONOLUA_STRING(MONOLUA_VERSION_PATCH)
#ifndef RC_INVOKED
inline constexpr const char* MONOLUA_PROTOCOL_VERSION = "MonoLua/" MONOLUA_VERSION_STRING;
#endif
