// One Lua VM, serialized execution and structured command errors.
#pragma once
#include "common.h"
#include "protocol.h"
struct lua_State;
using OutputCallback = std::function<void(const char*)>;

class LuaEngine
{
  public:
    // Commands, hooks and scheduled callbacks emit independent, nested output batches.
    class OutputCapture
    {
      public:
        OutputCapture();
        ~OutputCapture();
        OutputCapture(const OutputCapture&) = delete;
        OutputCapture& operator=(const OutputCapture&) = delete;

      private:
        OutputCapture* previous;
        std::string text;
        bool truncated = false;
        friend class LuaEngine;
    };
    struct ExecutionError
    {
        protocol::ErrorCategory category = protocol::ErrorCategory::Lua;
        int32_t line = -1;
        std::string message;
    };
    static LuaEngine& Instance();
    bool Init(OutputCallback output, void (*onFault)());
    void Shutdown();
    bool IsInitialized() const { return m_initialized.load(); }
    bool ExecuteString(const char* code, size_t length);
    bool ExecuteFile(const char* path);
    ExecutionError GetLastError() const;

    // State access and output emission require the caller to hold GetMutex().
    lua_State* GetState() const { return IsFaulted() ? nullptr : m_L; }
    void EmitOutput(const char* text);
    std::recursive_mutex& GetMutex() { return m_luaMutex; }

    // A native fault invalidates the entire session. Never close or reuse the damaged VM.
    bool IsFaulted() const { return bridge_lifecycle::g_sessionFaulted.load(); }
    int HandleNativeFault() noexcept;
    void CheckHealthy() const;
    static int RaiseBridgeError(lua_State* state, const char* format, ...);
    static int RaiseManagedError(lua_State* state, const char* message);
    static std::string ErrorText(lua_State* state, int index);

  private:
    LuaEngine() = default;
    ~LuaEngine();
    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;
    void Abort() noexcept;
    bool ExecuteBuffer(const char* buffer, size_t length, const char* name, bool includeLine);
    bool RunBuffer(const char* buffer, size_t length, const char* name, bool includeLine);
    bool RunProtected(const char* buffer, size_t length, const char* name, bool includeLine);
    void SetLastError(protocol::ErrorCategory category, int32_t line, const char* message);
    static int LuaPrint(lua_State* state);
    static void PrintReturnValues(lua_State* state, int count);
    lua_State* m_L = nullptr;
    std::atomic<bool> m_initialized{false};
    void (*m_onFault)() = nullptr;
    OutputCallback m_outputCb;
    mutable std::recursive_mutex m_luaMutex;
    ExecutionError m_lastError;
};
