// 单个 Lua VM 串行执行，并返回结构化命令错误。
#pragma once
#include "common.h"
#include "protocol.h"
struct lua_State;
using OutputCallback = std::function<void(const char*)>;

class LuaEngine
{
  public:
    // 命令、Hook 和调度回调分别维护可嵌套的输出批次。
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
    bool Init(OutputCallback output);
    void Shutdown();
    bool IsInitialized() const { return m_initialized.load(); }
    bool ExecuteString(const char* code, size_t length);
    bool ExecuteFile(const char* path);
    ExecutionError GetLastError() const;

    // 访问 Lua 状态和发送输出前，调用方必须持有 GetMutex()。
    lua_State* GetState() const { return IsFaulted() ? nullptr : m_L; }
    void EmitOutput(const char* text);
    std::recursive_mutex& GetMutex() { return m_luaMutex; }

    // 功能调用中的原生异常只记录当前错误；无法安全展开的异常才隔离会话。
    bool IsFaulted() const { return bridge_lifecycle::g_sessionFaulted.load(); }
    int HandleNativeFault(EXCEPTION_POINTERS* info = nullptr) noexcept;
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
    void ShutdownSeh();
    bool ExecuteBuffer(const char* buffer, size_t length, const char* name, bool includeLine);
    bool RunBuffer(const char* buffer, size_t length, const char* name, bool includeLine);
    bool RunBufferSeh(const char* buffer, size_t length, const char* name, bool includeLine);
    bool RunProtected(const char* buffer, size_t length, const char* name, bool includeLine);
    void SetLastError(protocol::ErrorCategory category, int32_t line, const char* message);
    static int LuaPrint(lua_State* state);
    static void PrintReturnValues(lua_State* state, int count);
    lua_State* m_L = nullptr;
    std::atomic<bool> m_initialized{false};
    OutputCallback m_outputCb;
    mutable std::recursive_mutex m_luaMutex;
    ExecutionError m_lastError;
    // 首个故障完成写入后发布；后续级联异常不能覆盖原始原因。
    // 0=无故障，1=正在写入，3=会话故障。
    std::atomic<int> m_nativeFaultState{0};
    char m_nativeFaultMessage[512]{};
};
