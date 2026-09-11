/**
 * protocol.h — Lune 与 MonoLua.dll 共享的通信契约
 *
 * 帧格式为 [1 字节类型][4 字节小端长度][N 字节负载]。这里仅定义双方
 * 必须一致的消息类型、常量和命名规则；具体 I/O 实现属于各自模块。
 */
#pragma once

#include "version.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace protocol
{
    // 消息类型
    enum MessageType : uint8_t
    {
        // DLL → Lune
        MSG_HELLO = 0x10, // 握手：负载为 protocol::VERSION
        MSG_READY = 0x11, // 初始化就绪：负载为状态描述文本
        MSG_LOG = 0x20,   // Lua 输出或 Hook 日志：负载为 UTF-8 文本
        MSG_ERROR = 0x21, // 错误信息：[类别][行号 LE][UTF-8 文本]
        MSG_OK = 0x22,    // 命令执行成功：无负载

        // Lune → DLL
        MSG_CMD = 0x30,  // 执行 Lua 代码：负载为 UTF-8 源码
        MSG_FILE = 0x31, // 执行 Lua 文件：负载为 UTF-8 文件路径

        // 双向
        MSG_EXIT = 0xFF, // 退出通知：无负载
    };
    // 协议常量
    constexpr size_t HEADER_SIZE = 5;
    constexpr uint32_t MAX_PAYLOAD = 1024u * 1024u;

    enum class ErrorCategory : uint8_t
    {
        Lua = 1,
        Il2Cpp = 2,
        CSharp = 3,
        Lune = 4,
        Mono = 5,
    };

    constexpr size_t ERROR_HEADER_SIZE = 5; // 类别 1 字节 + 行号 4 字节 LE

    inline bool EncodeErrorPayload(ErrorCategory category, int32_t line, const char* message,
                                   std::vector<uint8_t>& payload)
    {
        const size_t messageLength = message == nullptr ? 0 : std::strlen(message);
        if (messageLength > MAX_PAYLOAD - ERROR_HEADER_SIZE)
        {
            return false;
        }

        payload.assign(ERROR_HEADER_SIZE + messageLength, 0);
        payload[0] = static_cast<uint8_t>(category);

        const uint32_t encodedLine = static_cast<uint32_t>(line);
        payload[1] = static_cast<uint8_t>(encodedLine & 0xFF);
        payload[2] = static_cast<uint8_t>((encodedLine >> 8) & 0xFF);
        payload[3] = static_cast<uint8_t>((encodedLine >> 16) & 0xFF);
        payload[4] = static_cast<uint8_t>((encodedLine >> 24) & 0xFF);

        if (messageLength > 0)
        {
            std::memcpy(payload.data() + ERROR_HEADER_SIZE, message, messageLength);
        }
        return true;
    }

    // HELLO 帧中的版本标识。
    constexpr const char* VERSION = MONOLUA_PROTOCOL_VERSION;

    // 共享内存名称：前缀 + 目标进程 PID。
    constexpr const wchar_t* SHARED_MEM_PREFIX = L"MonoLua_Config_";

    // 共享内存大小足以容纳一个命名管道名称及结尾的零字符。
    constexpr size_t SHARED_MEM_SIZE = 512;

    static_assert(MAX_PAYLOAD <= UINT32_MAX);
} // namespace protocol
