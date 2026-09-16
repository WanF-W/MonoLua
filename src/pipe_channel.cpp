// 线协议与 Lune 的 MONO_PROFILE 一致，传输策略只在本模块内处理。
#include "pipe_channel.h"
#include <algorithm>

namespace
{
    constexpr DWORD CONNECT_TIMEOUT = 15000;
    constexpr size_t LOG_CHUNK_SIZE = 64 * 1024;
    constexpr size_t MAX_LOG_QUEUE = 1024;
    constexpr size_t MAX_LOG_QUEUE_BYTES = 4 * 1024 * 1024;

    bool IsPeerClosedError(DWORD error)
    {
        return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA ||
               error == ERROR_HANDLE_EOF;
    }

    int PipeNativeFaultFilter(EXCEPTION_POINTERS* info) noexcept
    {
        if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        // 日志线程没有可继续的业务状态；任何原生异常都只关闭日志能力，
        // 不允许它成为宿主进程的未处理异常。
        return EXCEPTION_EXECUTE_HANDLER;
    }

    HANDLE OpenPipe(const std::wstring& name)
    {
        const ULONGLONG deadline = GetTickCount64() + CONNECT_TIMEOUT;
        for (;;)
        {
            HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) return pipe;
            const DWORD error = GetLastError();
            if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return INVALID_HANDLE_VALUE;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return INVALID_HANDLE_VALUE;
            if (error == ERROR_PIPE_BUSY)
                WaitNamedPipeW(name.c_str(), static_cast<DWORD>(deadline - now));
            else
                Sleep(50);
        }
    }
} // namespace

PipeChannel& PipeChannel::Instance()
{
    static PipeChannel instance;
    return instance;
}

PipeChannel::~PipeChannel()
{
    try
    {
        if (bridge_lifecycle::g_processTerminating.load())
        {
            if (m_logThread.joinable()) m_logThread.detach();
            return;
        }
        ShutdownSeh();
    }
    catch (...)
    {
        OutputDebugStringA("[MonoLua] pipe cleanup failed\n");
    }
}

bool PipeChannel::ReadPipeName(std::wstring& name)
{
    wchar_t mappingName[128]{};
    swprintf_s(mappingName, L"%s%lu", protocol::SHARED_MEM_PREFIX, GetCurrentProcessId());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName);
    if (!mapping) return false;
    const auto* data =
        static_cast<const wchar_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, protocol::SHARED_MEM_SIZE));
    if (!data)
    {
        CloseHandle(mapping);
        return false;
    }
    const size_t capacity = protocol::SHARED_MEM_SIZE / sizeof(wchar_t);
    const size_t length = wcsnlen_s(data, capacity);
    if (length && length < capacity) name.assign(data, length);
    UnmapViewOfFile(data);
    CloseHandle(mapping);
    return !name.empty();
}

bool PipeChannel::Init()
{
    if (m_connected.load()) return true;
    Shutdown();
    // Lune 会在 HELLO 前保持共享内存映射；配置失败时不猜测旧端点。
    std::wstring name;
    if (!ReadPipeName(name)) return false;
    HANDLE pipe = OpenPipe(name);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr))
    {
        CloseHandle(pipe);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_pipe = pipe;
        m_connected.store(true);
    }
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logStop = false;
        m_logEnqueued = m_logCompleted = 0;
        m_logWriterFailed.store(false);
        m_logCleanupBlocked.store(false);
    }
    try
    {
        m_logThread = std::thread(&PipeChannel::LogWriterMain, this);
    }
    catch (...)
    {
        Shutdown();
        return false;
    }
    return true;
}

bool PipeChannel::Transfer(void* buffer, uint32_t length, bool writing, DWORD* failureError)
{
    if (failureError) *failureError = ERROR_SUCCESS;
    auto* bytes = static_cast<uint8_t*>(buffer);
    uint32_t offset = 0;
    while (offset < length)
    {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent)
        {
            if (failureError) *failureError = GetLastError();
            return false;
        }
        HANDLE pipe = INVALID_HANDLE_VALUE;
        DWORD transferred = 0;
        BOOL completed = FALSE;
        DWORD error = ERROR_OPERATION_ABORTED;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_connected.load())
            {
                pipe = m_pipe;
                completed = writing
                                ? WriteFile(pipe, bytes + offset, length - offset, &transferred, &operation)
                                : ReadFile(pipe, bytes + offset, length - offset, &transferred, &operation);
                if (!completed) error = GetLastError();
            }
        }
        if (!completed && error == ERROR_IO_PENDING)
        {
            // 管道写入不能因为工具自身的短超时主动断开；只有对端关闭或进程终止
            // 才让重叠操作结束。这样慢速前端不会被误判为断线。
            const DWORD wait = WaitForSingleObject(operation.hEvent, INFINITE);
            if (wait == WAIT_OBJECT_0)
            {
                completed = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
                if (!completed) error = GetLastError();
            }
            else
            {
                CancelIoEx(pipe, &operation);
                // OVERLAPPED 及其缓冲区必须保持有效，直到取消操作完成。
                completed = GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                if (!completed) error = GetLastError();
            }
        }
        CloseHandle(operation.hEvent);
        if (!completed || !transferred)
        {
            if (failureError) *failureError = error;
            return false;
        }
        offset += transferred;
    }
    return true;
}

void PipeChannel::Disconnect()
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_connected.store(false);
        if (m_pipe != INVALID_HANDLE_VALUE) CancelIoEx(m_pipe, nullptr);
    }
    m_logCondition.notify_all();
}

bool PipeChannel::CheckPeer()
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_connected.load() || m_pipe == INVALID_HANDLE_VALUE) return false;
    DWORD available = 0;
    if (PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr)) return true;
    const DWORD error = GetLastError();
    if (IsPeerClosedError(error))
    {
        m_connected.store(false);
        CancelIoEx(m_pipe, nullptr);
        m_logCondition.notify_all();
        return false;
    }
    // 查询本身暂时不可用时保持连接状态；下一次真正读写仍会给出权威结果。
    return m_connected.load();
}

bool PipeChannel::SendFrame(uint8_t type, const void* data, uint32_t length)
{
    if (length > protocol::MAX_PAYLOAD || (length && !data)) return false;
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_connected.load()) return false;
    uint8_t header[protocol::HEADER_SIZE] = {type};
    for (unsigned index = 0; index < 4; ++index)
        header[index + 1] = static_cast<uint8_t>(length >> (index * 8));
    DWORD failureError = ERROR_SUCCESS;
    if (!Transfer(header, sizeof(header), true, &failureError) ||
        !Transfer(const_cast<void*>(data), length, true, &failureError))
    {
        if (IsPeerClosedError(failureError)) Disconnect();
        return false;
    }
    return true;
}

bool PipeChannel::RecvFrame(uint8_t& type, std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lock(m_readMutex);
    if (!m_connected.load()) return false;
    uint8_t header[protocol::HEADER_SIZE]{};
    DWORD failureError = ERROR_SUCCESS;
    if (!Transfer(header, sizeof(header), false, &failureError))
    {
        if (IsPeerClosedError(failureError)) Disconnect();
        return false;
    }
    type = header[0];
    uint32_t length = 0;
    for (unsigned index = 0; index < 4; ++index)
        length |= static_cast<uint32_t>(header[index + 1]) << (index * 8);
    if (length > protocol::MAX_PAYLOAD)
    {
        // 未消费的载荷不能作为下一帧头读取；终止失去同步的连接。
        Disconnect();
        return false;
    }
    payload.resize(length);
    if (!Transfer(payload.data(), length, false, &failureError))
    {
        if (IsPeerClosedError(failureError)) Disconnect();
        return false;
    }
    return true;
}

bool PipeChannel::SendHello()
{
    return SendFrame(protocol::MSG_HELLO, protocol::VERSION,
                     static_cast<uint32_t>(strlen(protocol::VERSION)));
}

bool PipeChannel::SendReady(const char* message)
{
    if (!message) message = "ready";
    const size_t length = strnlen_s(message, protocol::MAX_PAYLOAD + 1);
    return length <= protocol::MAX_PAYLOAD && FlushLogs() &&
           SendFrame(protocol::MSG_READY, message, static_cast<uint32_t>(length));
}

bool PipeChannel::SendError(protocol::ErrorCategory category, int32_t line, const char* text)
{
    try
    {
        std::vector<uint8_t> payload;
        // 即使 Lua 错误过长也要返回响应，不能让 Lune 等待超时。
        if (!protocol::EncodeErrorPayload(category, line, text ? text : "unknown error", payload))
            protocol::EncodeErrorPayload(category, line, "error message exceeds the protocol limit", payload);
        return FlushLogs() &&
               SendFrame(protocol::MSG_ERROR, payload.data(), static_cast<uint32_t>(payload.size()));
    }
    catch (...)
    {
        // 内存分配失败时仍发送固定大小的错误帧，避免异常穿出工作线程。
        static constexpr char FALLBACK[] = "tool error (out of memory)";
        uint8_t payload[protocol::ERROR_HEADER_SIZE + sizeof(FALLBACK) - 1]{};
        payload[0] = static_cast<uint8_t>(category);
        const uint32_t encodedLine = static_cast<uint32_t>(line);
        payload[1] = static_cast<uint8_t>(encodedLine & 0xFF);
        payload[2] = static_cast<uint8_t>((encodedLine >> 8) & 0xFF);
        payload[3] = static_cast<uint8_t>((encodedLine >> 16) & 0xFF);
        payload[4] = static_cast<uint8_t>((encodedLine >> 24) & 0xFF);
        memcpy(payload + protocol::ERROR_HEADER_SIZE, FALLBACK, sizeof(FALLBACK) - 1);
        return SendFrame(protocol::MSG_ERROR, payload, static_cast<uint32_t>(sizeof(payload)));
    }
}

bool PipeChannel::SendOk()
{
    return FlushLogs() && SendFrame(protocol::MSG_OK, nullptr, 0);
}

bool PipeChannel::SendExit()
{
    return FlushLogs() && SendFrame(protocol::MSG_EXIT, nullptr, 0);
}

bool PipeChannel::SendLog(const char* text)
{
    if (!text || !m_connected.load() || m_logWriterFailed.load()) return false;
    const size_t length = strnlen_s(text, MAX_LOG_QUEUE_BYTES + 1);
    if (length > MAX_LOG_QUEUE_BYTES) { ++m_droppedLogs; return false; }
    const size_t chunks = length == 0 ? 1 : (length + LOG_CHUNK_SIZE - 4) / (LOG_CHUNK_SIZE - 3);
    bool accepted = true;
    try
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        if (m_logStop || !m_connected.load() || chunks > MAX_LOG_QUEUE - m_logQueue.size() ||
            length > MAX_LOG_QUEUE_BYTES - m_logQueuedBytes)
        {
            ++m_droppedLogs;
            return false;
        }
        size_t offset = 0;
        do
        {
            size_t bytes = (std::min)(LOG_CHUNK_SIZE, length - offset);
            // 保证一个 UTF-8 码点不跨帧，和 Lune 的文本解码规则一致。
            if (offset + bytes < length)
                while (bytes && (static_cast<unsigned char>(text[offset + bytes]) & 0xC0) == 0x80)
                    --bytes;
            if (!bytes && offset < length) bytes = (std::min)(LOG_CHUNK_SIZE, length - offset);
            m_logQueue.emplace_back(text + offset, bytes);
            m_logQueuedBytes += bytes;
            ++m_logEnqueued;
            offset += bytes;
        } while (offset < length);
    }
    catch (...)
    {
        accepted = false;
        ++m_droppedLogs;
    }
    // 如果部分分片已经入队后分配失败，也要唤醒写线程。
    m_logCondition.notify_all();
    return accepted;
}

bool PipeChannel::FlushLogs()
{
    // 日志线程发生原生故障时可能跳过互斥锁析构；不要再等待日志锁，
    // 直接放弃日志同步，主通道仍可继续发送命令结果。
    if (m_logCleanupBlocked.load()) return m_connected.load();
    std::unique_lock<std::mutex> lock(m_logMutex);
    if (m_logCleanupBlocked.load()) return m_connected.load();
    const uint64_t target = m_logEnqueued;
    m_logCondition.wait(lock, [this, target] {
        return m_logCompleted >= target || !m_connected.load() || m_logStop || m_logWriterFailed.load();
    });
    // 日志线程失败只关闭日志能力，主通道仍然必须能够发送命令响应。
    return m_connected.load() && (m_logCompleted >= target || m_logWriterFailed.load());
}

void PipeChannel::LogWriterMain()
{
    try
    {
        LogWriterMainSeh();
    }
    catch (...)
    {
        DisableLogWriter();
    }
}

void PipeChannel::ShutdownSeh()
{
    __try
    {
        Shutdown();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OutputDebugStringA("[MonoLua] pipe cleanup raised a native exception\n");
    }
}

void PipeChannel::LogWriterMainSeh()
{
    __try
    {
        LogWriterMainImpl();
    }
    __except (PipeNativeFaultFilter(GetExceptionInformation()))
    {
        MarkLogWriterFailed();
    }
}

void PipeChannel::DisableLogWriter() noexcept
{
    m_logWriterFailed.store(true);
    m_logCondition.notify_all();
}

void PipeChannel::MarkLogWriterFailed() noexcept
{
    m_logWriterFailed.store(true);
    m_logCleanupBlocked.store(true);
    m_logCondition.notify_all();
    OutputDebugStringA("[MonoLua] log writer raised a native exception; log cleanup deferred\n");
}

void PipeChannel::LogWriterMainImpl()
{
    try
    {
        for (;;)
        {
            std::string message;
            {
                std::unique_lock<std::mutex> lock(m_logMutex);
                m_logCondition.wait(lock,
                                    [this] { return m_logStop || !m_connected.load() || !m_logQueue.empty(); });
                if (m_logStop || !m_connected.load()) return;
                message = std::move(m_logQueue.front());
                m_logQueue.pop_front();
                m_logQueuedBytes -= message.size();
            }
            const bool sent = SendFrame(protocol::MSG_LOG, message.data(), static_cast<uint32_t>(message.size()));
            {
                std::lock_guard<std::mutex> lock(m_logMutex);
                ++m_logCompleted;
            }
            m_logCondition.notify_all();
            if (!sent)
            {
                DisableLogWriter();
                try
                {
                    std::lock_guard<std::mutex> lock(m_logMutex);
                    m_logQueue.clear();
                    m_logQueuedBytes = 0;
                    m_logCompleted = m_logEnqueued;
                    m_logStop = true;
                }
                catch (...)
                {
                }
                return;
            }
        }
    }
    catch (...)
    {
        // 日志线程故障不能让 FlushLogs 永久等待，也不能替主通道断线。
        DisableLogWriter();
        try
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            m_logQueue.clear();
            m_logQueuedBytes = 0;
            m_logCompleted = m_logEnqueued;
            m_logStop = true;
        }
        catch (...)
        {
        }
        m_logCondition.notify_all();
        OutputDebugStringA("[MonoLua] log writer failed; log output disabled\n");
    }
}

void PipeChannel::Shutdown()
{
    Disconnect();
    if (!m_logCleanupBlocked.load())
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logStop = true;
        m_logQueue.clear();
        m_logQueuedBytes = 0;
        m_logCondition.notify_all();
    }
    if (m_logThread.joinable()) m_logThread.join();
    // 原生故障可能发生在日志线程持有写锁的 I/O 调用中；此时不能再
    // 试图获取一个析构函数已被跳过的锁，句柄交给进程/DLL 回收。
    if (m_logCleanupBlocked.load()) return;
    std::scoped_lock ioLock(m_readMutex, m_writeMutex);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(m_pipe, INVALID_HANDLE_VALUE));
}
