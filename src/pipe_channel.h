// Full-duplex Lune client. One reader, serialized frames, bounded asynchronous logs.
#pragma once
#include "common.h"
#include "protocol.h"

class PipeChannel
{
  public:
    static PipeChannel& Instance();
    // Lifecycle is owned by the DLL worker. Disconnect may be called from any thread.
    bool Init();
    void Shutdown();
    void Disconnect();
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
    bool Transfer(void* buffer, uint32_t length, bool writing);
    bool SendFrame(uint8_t type, const void* data, uint32_t length);
    bool FlushLogs();
    void LogWriterMain();

    // Submission and cancellation share the state lock. Close waits for both I/O owners,
    // so no OVERLAPPED operation can outlive its HANDLE or start after cancellation.
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
    bool m_logStop = true;
    std::thread m_logThread;
};
