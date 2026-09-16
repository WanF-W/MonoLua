// 全双工 Lune 客户端：单读者、串行帧和有界异步日志。
#pragma once
#include "common.h"
#include "protocol.h"

class PipeChannel
{
  public:
    static PipeChannel& Instance();
    // 生命周期由 DLL 工作线程管理；Disconnect 可以从任意线程调用。
    bool Init();
    void Shutdown();
    void Disconnect();
    bool IsConnected() const { return m_connected.load(); }
    bool CheckPeer();
    bool SendHello();
    bool SendReady(const char* message);
    bool SendLog(const char* text);
    uint64_t DroppedLogs() const { return m_droppedLogs.load(); }
    bool SendError(protocol::ErrorCategory category, int32_t line, const char* text);
    bool SendOk();
    bool SendExit();
    bool RecvFrame(uint8_t& type, std::vector<uint8_t>& payload);

  private:
    PipeChannel() = default;
    ~PipeChannel();
    PipeChannel(const PipeChannel&) = delete;
    PipeChannel& operator=(const PipeChannel&) = delete;

    bool ReadPipeName(std::wstring& name);
    bool Transfer(void* buffer, uint32_t length, bool writing, DWORD* failureError = nullptr);
    bool SendFrame(uint8_t type, const void* data, uint32_t length);
    bool FlushLogs();
    void LogWriterMain();
    void ShutdownSeh();
    void LogWriterMainSeh();
    void LogWriterMainImpl();
    void DisableLogWriter() noexcept;
    void MarkLogWriterFailed() noexcept;

    // 提交和取消共用状态锁。关闭时等待两个 I/O 所有者，确保 OVERLAPPED
    // 操作不会在 HANDLE 关闭后继续存在，也不会在取消后重新启动。
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::atomic<bool> m_connected{false};
    std::mutex m_stateMutex;
    std::mutex m_readMutex;
    std::mutex m_writeMutex;
    std::mutex m_logMutex;
    std::condition_variable m_logCondition;
    std::deque<std::string> m_logQueue;
    size_t m_logQueuedBytes = 0;
    uint64_t m_logEnqueued = 0;
    uint64_t m_logCompleted = 0;
    std::atomic<uint64_t> m_droppedLogs{0};
    std::atomic<bool> m_logWriterFailed{false};
    std::atomic<bool> m_logCleanupBlocked{false};
    bool m_logStop = true;
    std::thread m_logThread;
};
