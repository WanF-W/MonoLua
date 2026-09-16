// 单个 Lua VM 串行执行；普通 Lua/功能错误只影响当前命令，无法安全展开的
// Hook 或工作线程原生故障才会隔离会话。
#include "lua_engine.h"
#include "lua_bridge.h"
#include <cctype>
#include <charconv>
#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <exception>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace
{
    thread_local LuaEngine::OutputCapture* g_outputCapture = nullptr;

    void InvokeOutputCallbackSeh(OutputCallback& callback, const char* text) noexcept
    {
        __try
        {
            callback(text);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutputDebugStringA("[MonoLua] output callback raised a native exception\n");
        }
    }
    void ResetOutputCaptureAfterNativeFault() noexcept
    {
        g_outputCapture = nullptr;
    }
    constexpr const char* ErrorMetatable = "MonoLua.Error";
    struct TaggedError { protocol::ErrorCategory category; };
    int ErrorToString(lua_State* state)
    {
        lua_getiuservalue(state, 1, 1);
        return 1;
    }
    int ThrowTaggedError(lua_State* state, protocol::ErrorCategory category)
    {
        // 消息已经在栈顶。把分类标签保存在错误对象上，pcall/error 可以保留它，
        // 已捕获的错误不会污染后续错误。
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
    try
    {
        auto& engine = LuaEngine::Instance();
        if (!engine.IsFaulted() && !text.empty() && engine.m_outputCb)
        {
            InvokeOutputCallbackSeh(engine.m_outputCb, text.c_str());
        }
    }
    catch (...)
    {
        OutputDebugStringA("[MonoLua] output callback failed\n");
    }
}

void LuaEngine::EmitOutput(const char* text)
{
    // 会话隔离后不再进入 Lua；错误报告仍可通过独立的管道回调送到 Lune。
    if (!text) return;
    if (g_outputCapture)
    {
        // 一条命令可能输出多个受限 dump；同时限制整批大小，避免超过传输
        // 上限后整条日志被丢弃。
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
    {
        try
        {
            InvokeOutputCallbackSeh(m_outputCb, text);
        }
        catch (...)
        {
            OutputDebugStringA("[MonoLua] output callback failed\n");
        }
    }
}

LuaEngine& LuaEngine::Instance()
{
    static LuaEngine instance;
    return instance;
}

LuaEngine::~LuaEngine()
{
    if (!bridge_lifecycle::g_processTerminating.load() && !IsFaulted())
    {
        try
        {
            ShutdownSeh();
        }
        catch (...)
        {
            OutputDebugStringA("[MonoLua] Lua cleanup failed\n");
        }
    }
}

void LuaEngine::Abort() noexcept
{
    m_initialized.store(false);
    bridge_lifecycle::g_sessionFaulted.store(true);
}

void LuaEngine::ShutdownSeh()
{
    __try
    {
        Shutdown();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OutputDebugStringA("[MonoLua] Lua cleanup raised a native exception\n");
    }
}

int LuaEngine::HandleNativeFault(EXCEPTION_POINTERS* info) noexcept
{
    // A fault escaping the native-operation boundary may have bypassed Lua's
    // errorJmp/CallInfo restoration. Never reuse that VM.
    int expected = 0;
    if (m_nativeFaultState.compare_exchange_strong(expected, 1))
    {
        const auto* record = info ? info->ExceptionRecord : nullptr;
        sprintf_s(m_nativeFaultMessage,
            "%s: native exception 0x%08lX at %p; MonoLua session quarantined",
            bridge_lifecycle::g_nativeStage, record ? record->ExceptionCode : 0,
            record ? record->ExceptionAddress : nullptr);
        OutputDebugStringA(m_nativeFaultMessage);
        OutputDebugStringA("\n");
        m_nativeFaultState.store(3);
    }
    ResetOutputCaptureAfterNativeFault();
    Abort();
    return EXCEPTION_EXECUTE_HANDLER;
}

void LuaEngine::CheckHealthy() const
{
    // 把嵌套 Hook 故障传给外层原生边界，不直接触碰 Lua。
    if (IsFaulted()) RaiseException(bridge_lifecycle::SESSION_FAULT_CODE, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

LuaEngine::ExecutionError LuaEngine::GetLastError() const
{
    const int nativeFault = m_nativeFaultState.load();
    if (nativeFault == 3 || IsFaulted())
        return {protocol::ErrorCategory::Mono, -1, m_nativeFaultMessage[0]
            ? m_nativeFaultMessage : "native exception; MonoLua session quarantined"};
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    return m_lastError;
}

void LuaEngine::SetLastError(protocol::ErrorCategory category, int32_t line, const char* message)
{
    if (IsFaulted()) return;
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    m_lastError = {category, line, message ? message : "unknown error"};
}

bool LuaEngine::Init(OutputCallback output)
{
    std::lock_guard<std::recursive_mutex> lock(m_luaMutex);
    if (IsFaulted()) return false;
    if (IsInitialized()) return true;
    m_L = luaL_newstate();
    if (!m_L) return false;
    try
    {
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
    }
    catch (...)
    {
        try
        {
            if (m_L) lua_close(m_L);
        }
        catch (...)
        {
        }
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
    bridge_lifecycle::ClearNativeCallFault();
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
    const bool success = RunProtected(buffer, length, name, includeLine);
    // SEH 处理器只写入固定缓冲区，不能在过滤器里操作 std::string。
    // 在这里把当前命令的原生故障转换成正常的结构化错误，避免 Lune
    // 收到空错误后误以为连接异常。
    const int nativeFaultState = m_nativeFaultState.load();
    if ((nativeFaultState == 3 || IsFaulted()) && m_lastError.message.empty())
    {
        try
        {
            m_lastError.category = protocol::ErrorCategory::Mono;
            m_lastError.line = -1;
            m_lastError.message = m_nativeFaultMessage[0]
                ? m_nativeFaultMessage
                : "native exception; MonoLua session quarantined";
        }
        catch (...)
        {
            // 错误文本分配失败也不能把命令线程推出工作循环；SendError
            // 会在没有文本时使用固定回退消息。
            m_lastError.category = protocol::ErrorCategory::Mono;
            m_lastError.line = -1;
        }
    }
    if (bridge_lifecycle::g_nativeCallFaulted)
    {
        char message[128]{};
        if (bridge_lifecycle::g_nativeCallFaultCode)
            sprintf_s(message, "native exception during tool operation (0x%08lX)",
                      bridge_lifecycle::g_nativeCallFaultCode);
        else
            sprintf_s(message, "native call raised a C++ exception during tool operation");
        m_lastError.category = protocol::ErrorCategory::Mono;
        m_lastError.line = -1;
        m_lastError.message = message;
        bridge_lifecycle::ClearNativeCallFault();
        return false;
    }
    return success;
}

bool LuaEngine::RunBufferSeh(const char* buffer, size_t length, const char* name, bool includeLine)
{
    __try
    {
        return RunBuffer(buffer, length, name, includeLine);
    }
    __except (HandleNativeFault(GetExceptionInformation()))
    {
        return false;
    }
}

bool LuaEngine::RunProtected(const char* buffer, size_t length, const char* name, bool includeLine)
{
    try
    {
        return RunBufferSeh(buffer, length, name, includeLine);
    }
    catch (...)
    {
        // An exception outside lua_pcall also lacks a proven VM recovery path.
        HandleNativeFault(nullptr);
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
    // Lune 统一只为文件显示行号。
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
    // 磁盘 I/O 在 Lua 锁外执行；ExecuteBuffer 会再次检查会话状态。
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
    // '=' 表示字面量源名称，而不是 Lua 源代码文本。
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
