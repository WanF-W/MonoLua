// The wire contract matches Lune's MONO_PROFILE. Transport policy stays local to this module.
#include "pipe_channel.h"
#include <algorithm>

namespace
{
    constexpr DWORD CONNECT_TIMEOUT = 15000;
    constexpr DWORD WRITE_TIMEOUT = 2000;
    constexpr size_t LOG_CHUNK_SIZE = 64 * 1024;
    constexpr size_t MAX_LOG_QUEUE = 1024;
    constexpr size_t MAX_LOG_QUEUE_BYTES = 4 * 1024 * 1024;

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
    if (bridge_lifecycle::g_processTerminating.load())
    {
        if (m_logThread.joinable()) m_logThread.detach();
        return;
    }
    Shutdown();
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
    // Lune holds the mapping through HELLO. Do not guess a legacy endpoint on configuration failure.
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

bool PipeChannel::Transfer(void* buffer, uint32_t length, bool writing)
{
    auto* bytes = static_cast<uint8_t*>(buffer);
    uint32_t offset = 0;
    while (offset < length)
    {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
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
            const DWORD wait = WaitForSingleObject(operation.hEvent, writing ? WRITE_TIMEOUT : INFINITE);
            if (wait == WAIT_OBJECT_0)
                completed = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
            else
            {
                CancelIoEx(pipe, &operation);
                // OVERLAPPED and its buffer must remain alive until cancellation completes.
                GetOverlappedResult(pipe, &operation, &transferred, TRUE);
            }
        }
        CloseHandle(operation.hEvent);
        if (!completed || !transferred) return false;
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

bool PipeChannel::SendFrame(uint8_t type, const void* data, uint32_t length)
{
    if (length > protocol::MAX_PAYLOAD || (length && !data)) return false;
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_connected.load()) return false;
    uint8_t header[protocol::HEADER_SIZE] = {type};
    for (unsigned index = 0; index < 4; ++index)
        header[index + 1] = static_cast<uint8_t>(length >> (index * 8));
    if (!Transfer(header, sizeof(header), true) || !Transfer(const_cast<void*>(data), length, true))
    {
        Disconnect();
        return false;
    }
    return true;
}

bool PipeChannel::RecvFrame(uint8_t& type, std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lock(m_readMutex);
    if (!m_connected.load()) return false;
    uint8_t header[protocol::HEADER_SIZE]{};
    if (!Transfer(header, sizeof(header), false))
    {
        Disconnect();
        return false;
    }
    type = header[0];
    uint32_t length = 0;
    for (unsigned index = 0; index < 4; ++index)
        length |= static_cast<uint32_t>(header[index + 1]) << (index * 8);
    if (length > protocol::MAX_PAYLOAD)
    {
        Disconnect();
        return false;
    }
    payload.resize(length);
    if (!Transfer(payload.data(), length, false))
    {
        Disconnect();
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
    std::vector<uint8_t> payload;
    // Even an oversized Lua error must receive a response, rather than timing out in Lune.
    if (!protocol::EncodeErrorPayload(category, line, text ? text : "unknown error", payload))
        protocol::EncodeErrorPayload(category, line, "error message exceeds the protocol limit", payload);
    return FlushLogs() &&
           SendFrame(protocol::MSG_ERROR, payload.data(), static_cast<uint32_t>(payload.size()));
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
    if (!text || !m_connected.load()) return false;
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
            // Keep UTF-8 code points within one frame, matching Lune's text decoding.
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
    // Also wake the writer if allocation failed after some chunks had already been queued.
    m_logCondition.notify_all();
    return accepted;
}

bool PipeChannel::FlushLogs()
{
    std::unique_lock<std::mutex> lock(m_logMutex);
    const uint64_t target = m_logEnqueued;
    m_logCondition.wait(
        lock, [this, target] { return m_logCompleted >= target || !m_connected.load() || m_logStop; });
    return m_connected.load() && m_logCompleted >= target;
}

void PipeChannel::LogWriterMain()
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
        if (!sent) return;
    }
}

void PipeChannel::Shutdown()
{
    Disconnect();
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logStop = true;
        m_logQueue.clear();
        m_logQueuedBytes = 0;
    }
    m_logCondition.notify_all();
    if (m_logThread.joinable()) m_logThread.join();
    std::scoped_lock ioLock(m_readMutex, m_writeMutex);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(m_pipe, INVALID_HANDLE_VALUE));
}
