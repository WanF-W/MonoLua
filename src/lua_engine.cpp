// One serialized Lua VM. Ordinary Lua errors unwind C++ owners; native faults end the session.
#include "lua_engine.h"
#include "lua_bridge.h"
#include <cctype>
#include <charconv>
#include <cstdarg>
#include <algorithm>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace
{
    thread_local LuaEngine::OutputCapture* g_outputCapture = nullptr;
    constexpr const char* ErrorMetatable = "MonoLua.Error";
    struct TaggedError { protocol::ErrorCategory category; };
    int ErrorToString(lua_State* state)
    {
        lua_getiuservalue(state, 1, 1);
        return 1;
    }
    int ThrowTaggedError(lua_State* state, protocol::ErrorCategory category)
    {
        // The message is already on top. Keeping the tag on the error object means
        // pcall/error preserves it; caught errors cannot contaminate a later failure.
        auto* error = static_cast<TaggedError*>(lua_newuserdatauv(state, sizeof(TaggedError), 1));
        error->category = category;
        lua_pushvalue(state, -2);
        lua_setiuservalue(state, -2, 1);
        if (luaL_newmetatable(state, ErrorMetatable))
        {
            lua_pushcfunction(state, ErrorToString);
            lua_setfield(state, -2, "__tostring");
            lua_pushliteral(state, "MonoLua error");
            lua_setfield(state, -2, "__metatable");
        }
        lua_setmetatable(state, -2);
        return lua_error(state);
    }
    bool StripLuaSourcePrefix(std::string& message, int32_t& line)
    {
        const size_t marker = message.rfind("[string ", 0) == 0 ? message.find("]:") : message.find(':');
        if (marker == std::string::npos) return false;
        const size_t start = marker + (message[marker] == ']' ? 2 : 1);
        size_t end = start;
        while (end < message.size() && std::isdigit(static_cast<unsigned char>(message[end])))
            ++end;
        if (end == start || end == message.size() || message[end] != ':') return false;
        int32_t parsed = -1;
        if (std::from_chars(message.data() + start, message.data() + end, parsed).ec != std::errc{})
            return false;
        line = parsed;
        ++end;
        if (end < message.size() && message[end] == ' ') ++end;
        message.erase(0, end);
        return true;
    }
} // namespace

int LuaEngine::RaiseBridgeError(lua_State* state, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    lua_pushvfstring(state, format, arguments);
    va_end(arguments);
    return ThrowTaggedError(state, protocol::ErrorCategory::Mono);
}

int LuaEngine::RaiseManagedError(lua_State* state, const char* message)
{
    lua_pushstring(state, message ? message : "unknown managed exception");
    return ThrowTaggedError(state, protocol::ErrorCategory::CSharp);
}

std::string LuaEngine::ErrorText(lua_State* state, int index)
{
    const bool tagged = luaL_testudata(state, index, ErrorMetatable) != nullptr;
    if (tagged) { lua_getiuservalue(state, index, 1); index = -1; }
    size_t length = 0;
    const char* value = lua_tolstring(state, index, &length);
    std::string text = value ? std::string(value, length) : "(non-string error object)";
    if (tagged) lua_pop(state, 1);
    return text;
}

LuaEngine::OutputCapture::OutputCapture() : previous(g_outputCapture)
{
    g_outputCapture = this;
}

LuaEngine::OutputCapture::~OutputCapture()
{
    g_outputCapture = previous;
    auto& engine = LuaEngine::Instance();
    if (!engine.IsFaulted() && !text.empty() && engine.m_outputCb) engine.m_outputCb(text.c_str());
}

void LuaEngine::EmitOutput(const char* text)
{
    if (!text) return;
    if (g_outputCapture)
    {
        // One command can emit many bounded dumps. Bound the batch as well so it
        // cannot grow past the transport limit and disappear as a single log.
        constexpr size_t budget = 1024 * 1024;
        auto& capture = *g_outputCapture;
        if (capture.truncated) return;
        const size_t room = budget - capture.text.size();
        const size_t length = strnlen_s(text, room + 1);
        capture.text.append(text, (std::min)(length, room));
        if (length > room)
        {
            capture.text += "\n... <output batch truncated>\n";
            capture.truncated = true;
        }
    }
    else if (m_outputCb)
        m_outputCb(text);
}

LuaEngine& LuaEngine::Instance()
{
    static LuaEngine instance;
    return instance;
}

LuaEngine::~LuaEngine()
{
    if (!bridge_lifecycle::g_processTerminating.load() && !IsFaulted()) Shutdown();
}

void LuaEngine::Abort() noexcept
{
    m_initialized.store(false);
    if (!bridge_lifecycle::g_sessionFaulted.exchange(true) && m_onFault) m_onFault();
}

int LuaEngine::HandleNativeFault() noexcept
{
    // Filters run before stack unwinding: release native locks, but retain Mono roots
    // and the VM instead of invoking a potentially damaged runtime during cleanup.
    Abort();
    return EXCEPTION_EXECUTE_HANDLER;
}

void LuaEngine::CheckHealthy() const
{
    // Propagate a nested Hook fault to the enclosing native execution boundary without touching Lua.
    if (IsFaulted()) RaiseException(bridge_lifecycle::SESSION_FAULT_CODE, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

LuaEngine::ExecutionError LuaEngine::GetLastError() const
{
    if (IsFaulted())
        return {protocol::ErrorCategory::Mono, -1, "native exception; MonoLua session stopped"};
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    return m_lastError;
}

void LuaEngine::SetLastError(protocol::ErrorCategory category, int32_t line, const char* message)
{
    if (IsFaulted()) return;
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    m_lastError = {category, line, message ? message : "unknown error"};
}

bool LuaEngine::Init(OutputCallback output, void (*onFault)())
{
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    if (IsFaulted()) return false;
    if (IsInitialized()) return true;
    m_onFault = onFault;
    m_L = luaL_newstate();
    if (!m_L) return false;
    m_outputCb = std::move(output);
    luaL_openlibs(m_L);
    lua_pushcfunction(m_L, LuaPrint);
    lua_setglobal(m_L, "print");
    if (!LuaBridge_Init(m_L))
    {
        lua_close(m_L);
        m_L = nullptr;
        m_outputCb = nullptr;
        return false;
    }
    m_initialized.store(true);
    return true;
}

void LuaEngine::Shutdown()
{
    if (IsFaulted()) return;
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    if (IsFaulted()) return;
    lua_State* state = std::exchange(m_L, nullptr);
    if (state) lua_close(state);
    m_outputCb = nullptr;
    m_initialized.store(false);
}

bool LuaEngine::ExecuteBuffer(const char* buffer, size_t length, const char* name, bool includeLine)
{
    bridge_lifecycle::ManagedCallScope callScope;
    if (IsFaulted()) return false;
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    if (IsFaulted()) return false;
    m_lastError = {};
    if (!IsInitialized() || !m_L)
    {
        m_lastError.message = "Lua engine is not initialized";
        return false;
    }
    if (!buffer)
    {
        m_lastError.message = "empty Lua input";
        return false;
    }
    return RunProtected(buffer, length, name, includeLine);
}

bool LuaEngine::RunProtected(const char* buffer, size_t length, const char* name, bool includeLine)
{
    // Keep SEH outside the C++ locals in RunBuffer. An escaped native fault skips Lua's own
    // call-frame/error-chain restoration, so no lua_settop or lua_close is safe afterwards.
    __try
    {
        return RunBuffer(buffer, length, name, includeLine);
    }
    __except (HandleNativeFault())
    {
        return false;
    }
}

bool LuaEngine::RunBuffer(const char* buffer, size_t length, const char* name, bool includeLine)
{
    OutputCapture outputCapture;
    lua_State* state = m_L;
    const int baseline = lua_gettop(state);
    int status = luaL_loadbuffer(state, buffer, length, name);
    if (status == LUA_OK) status = lua_pcall(state, 0, LUA_MULTRET, 0);
    CheckHealthy();
    if (status != LUA_OK)
    {
        if (auto* error = static_cast<TaggedError*>(luaL_testudata(state, -1, ErrorMetatable)))
        {
            m_lastError.category = error->category;
            lua_getiuservalue(state, -1, 1);
        }
        const char* text = lua_tostring(state, -1);
        std::string message = text ? text : "(non-string error object)";
        int32_t line = -1;
        if (m_lastError.category == protocol::ErrorCategory::Lua) StripLuaSourcePrefix(message, line);
        m_lastError.line = includeLine && m_lastError.category == protocol::ErrorCategory::Lua ? line : -1;
        m_lastError.message = std::move(message);
        lua_settop(state, baseline);
        return false;
    }
    PrintReturnValues(state, lua_gettop(state) - baseline);
    CheckHealthy();
    lua_settop(state, baseline);
    return true;
}

bool LuaEngine::ExecuteString(const char* code, size_t length)
{
    // Unified Lune displays line numbers for files only.
    return ExecuteBuffer(code, length, "=lune", false);
}

bool LuaEngine::ExecuteFile(const char* path)
{
    if (IsFaulted()) return false;
    auto error = [this](const char* message) {
        SetLastError(protocol::ErrorCategory::Lua, -1, message);
        return false;
    };
    if (!path || !*path) return error("Lua file path is empty");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (length <= 0) return error("invalid UTF-8 file path");
    std::vector<wchar_t> widePath(static_cast<size_t>(length));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, widePath.data(), length) <= 0)
        return error("failed to convert file path");
    // Disk I/O runs outside the Lua mutex; ExecuteBuffer rechecks session state.
    std::vector<char> source;
    {
        struct FileHandle
        {
            HANDLE value;
            ~FileHandle()
            {
                if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
            }
        } file{CreateFileW(widePath.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) return error("cannot open file");
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0) return error("cannot get file size");
        if (size.QuadPart > 4 * 1024 * 1024) return error("file too large (max 4MB)");
        source.resize(static_cast<size_t>(size.QuadPart));
        size_t offset = 0;
        while (offset < source.size())
        {
            DWORD read = 0;
            if (!ReadFile(file.value, source.data() + offset, static_cast<DWORD>(source.size() - offset),
                          &read, nullptr) ||
                read == 0)
                return error("failed to read complete file");
            offset += read;
        }
    }
    const char* name = path;
    for (const char* current = path; *current; ++current)
        if (*current == '/' || *current == '\\') name = current + 1;
    // '=' makes this a literal source name rather than Lua source text.
    const std::string chunkName = std::string("=") + name;
    return ExecuteBuffer(source.empty() ? "" : source.data(), source.size(), chunkName.c_str(), true);
}

int LuaEngine::LuaPrint(lua_State* state)
{
    const int count = lua_gettop(state);
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    for (int index = 1; index <= count; ++index)
    {
        luaL_tolstring(state, index, nullptr);
        luaL_addvalue(&buffer);
        if (index < count) luaL_addchar(&buffer, '\t');
    }
    luaL_addchar(&buffer, '\n');
    luaL_pushresult(&buffer);
    Instance().EmitOutput(lua_tostring(state, -1));
    return 0;
}

void LuaEngine::PrintReturnValues(lua_State* state, int count)
{
    const int first = lua_gettop(state) - count + 1;
    std::string text;
    for (int index = first; index < first + count; ++index)
    {
        std::string value;
        const bool success = LuaBridge_TryToString(state, index, value);
        Instance().CheckHealthy();
        text += success ? value : "<tostring error: " + value + ">";
        text += '\n';
    }
    if (!text.empty()) Instance().EmitOutput(text.c_str());
}
