#include "bridge_session.h"
// DLL 组合入口：Lune 通信、Mono、Lua、命令循环和有序关闭。
#include "common.h"
#include "lua_engine.h"
#include "mono_hook.h"
#include "mono_runtime.h"
#include "mono_resolver.h"
#include "mono_scheduler.h"
#include "pipe_channel.h"
#include <exception>
#include <cstdio>

static std::atomic<bool> g_unloadBlocked{false};

static void WaitForFrontendExit() noexcept;
static int WorkerExceptionFilter(EXCEPTION_POINTERS* info) noexcept;

static void QuarantineWorker() noexcept
{
    // 原生异常可能跳过当前线程持有的互斥锁析构；此时不能再访问
    // PipeChannel、Lua 或 Mono。保持 DLL 映射，直到游戏进程真正结束。
    g_unloadBlocked.store(true);
    while (!bridge_lifecycle::g_processTerminating.load()) Sleep(250);
}

static void RecoverAfterWorkerExceptionSeh()
{
    __try
    {
        if (PipeChannel::Instance().IsConnected())
            PipeChannel::Instance().SendError(protocol::ErrorCategory::Mono, -1, "tool worker failed");
        WaitForFrontendExit();
        PipeChannel::Instance().Shutdown();
    }
    __except (WorkerExceptionFilter(GetExceptionInformation()))
    {
        QuarantineWorker();
    }
}

static void RecoverAfterWorkerException() noexcept
{
    try
    {
        RecoverAfterWorkerExceptionSeh();
    }
    catch (...)
    {
        QuarantineWorker();
    }
}

static int WorkerExceptionFilter(EXCEPTION_POINTERS* info) noexcept
{
    if (info && info->ExceptionRecord)
    {
        char message[160]{};
        sprintf_s(message, "[MonoLua] worker native exception 0x%08lX at %p; session quarantined\n",
                  info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
        OutputDebugStringA(message);
    }
    return LuaEngine::Instance().HandleNativeFault(info);
}

class ScopedMonoAttach final
{
  public:
    explicit ScopedMonoAttach(bool attach)
        : m_thread(attach ? MonoResolver::Instance().AttachThread() : nullptr)
    {
    }
    ~ScopedMonoAttach()
    {
        if (m_thread && !LuaEngine::Instance().IsFaulted()) MonoResolver::Instance().DetachThread(m_thread);
    }
    ScopedMonoAttach(const ScopedMonoAttach&) = delete;
    ScopedMonoAttach& operator=(const ScopedMonoAttach&) = delete;
    bool IsAttached() const { return m_thread != nullptr; }

  private:
    MonoThread* m_thread;
};

static void ShutdownBridge()
{
    // 原生故障后锁或 VM 帧可能已经损坏。保留运行时和 Hook 资源，后续
    // Hook 入口绕过 Lua；进程结束时由系统统一回收。
    if (!LuaEngine::Instance().IsFaulted())
    {
        {
            auto& runtime = MonoRuntime::Instance();
            ScopedMonoAttach cleanupAttach(runtime.IsInitialized() && !runtime.HasInitializationThread());
            const bool hooksStopped = MonoHook::Shutdown();
            if (!hooksStopped)
            {
                g_unloadBlocked.store(true);
                PipeChannel::Instance().Shutdown();
                return;
            }
            if (!LuaEngine::Instance().IsFaulted())
            {
                MonoScheduler::Shutdown();
                LuaEngine::Instance().Shutdown();
            }
        }
        if (!LuaEngine::Instance().IsFaulted()) MonoRuntime::Instance().Shutdown();
    }
    PipeChannel::Instance().Shutdown();
}

static void WorkerMain()
{
    auto& pipe = PipeChannel::Instance();
    while (!bridge_lifecycle::g_processTerminating.load() && !pipe.IsConnected())
    {
        if (pipe.Init()) break;
        // 注入时前端可能还在创建共享内存；初始化失败只重试，不能把 DLL
        // 当成一次性命令行程序自行卸载。
        Sleep(100);
    }
    if (!pipe.IsConnected()) return;
    if (!pipe.SendHello())
    {
        if (bridge_lifecycle::g_processTerminating.load()) return;
        if (pipe.IsConnected())
        {
            g_unloadBlocked.store(true);
            WaitForFrontendExit();
        }
        pipe.Shutdown();
        return;
    }
    auto& runtime = MonoRuntime::Instance();
    bool runtimeReady = false;
    while (!bridge_lifecycle::g_processTerminating.load() && pipe.IsConnected())
    {
        if (!pipe.CheckPeer()) break;
        bool initialized = false;
        try
        {
            initialized = runtime.Init();
        }
        catch (const std::exception& exception)
        {
            OutputDebugStringA("[MonoLua] Mono initialization raised a C++ exception\n");
            if (exception.what()) OutputDebugStringA(exception.what());
            OutputDebugStringA("\n");
            try { runtime.Shutdown(); } catch (...) {}
        }
        catch (...)
        {
            OutputDebugStringA("[MonoLua] Mono initialization raised an unknown exception\n");
            try { runtime.Shutdown(); } catch (...) {}
        }
        if (initialized)
        {
            runtimeReady = true;
            break;
        }
        Sleep(500);
    }
    if (!runtimeReady)
    {
        return;
    }

    auto& lua = LuaEngine::Instance();
    while (!bridge_lifecycle::g_processTerminating.load() && pipe.IsConnected())
    {
        if (!pipe.CheckPeer()) break;
        bool initialized = false;
        try
        {
            initialized = lua.Init([&pipe](const char* text) { pipe.SendLog(text); });
        }
        catch (const std::exception& exception)
        {
            OutputDebugStringA("[MonoLua] Lua initialization raised a C++ exception\n");
            if (exception.what()) OutputDebugStringA(exception.what());
            OutputDebugStringA("\n");
        }
        catch (...)
        {
            OutputDebugStringA("[MonoLua] Lua initialization raised an unknown exception\n");
        }
        if (initialized) break;
        Sleep(500);
    }
    if (!lua.IsInitialized()) return;
    std::string tickError;
    try
    {
        MonoScheduler::AutoSetTick(tickError);
    }
    catch (const std::exception& exception)
    {
        OutputDebugStringA("[MonoLua] scheduler initialization raised a C++ exception\n");
        OutputDebugStringA(exception.what() ? exception.what() : "unknown C++ exception");
        OutputDebugStringA("\n");
    }
    catch (...)
    {
        OutputDebugStringA("[MonoLua] scheduler initialization raised an unknown exception\n");
    }
    // READY 只表示运行时和 Lua 已经可以通信；调度器诊断在握手完成后单独发送。
    if (!pipe.SendReady("ready"))
    {
        if (bridge_lifecycle::g_processTerminating.load()) return;
        if (pipe.IsConnected())
        {
            g_unloadBlocked.store(true);
            WaitForFrontendExit();
            pipe.Shutdown();
        }
        else
            ShutdownBridge();
        return;
    }
    try
    {
        MonoScheduler::FlushDiagnostics();
    }
    catch (...)
    {
        OutputDebugStringA("[MonoLua] scheduler diagnostics could not be flushed\n");
    }

    runtime.DetachInitializationThread();
    bool frontendRequestedExit = false;
    while (!bridge_lifecycle::g_processTerminating.load() && pipe.IsConnected())
    {
        try
        {
            uint8_t type = 0;
            std::vector<uint8_t> payload;
            if (!pipe.RecvFrame(type, payload))
            {
                if (pipe.IsConnected())
                {
                    Sleep(50);
                    continue;
                }
                break;
            }
            if (type == protocol::MSG_EXIT)
            {
                frontendRequestedExit = true;
                pipe.SendExit();
                break;
            }
            if (type != protocol::MSG_CMD && type != protocol::MSG_FILE)
            {
                if (!pipe.SendError(protocol::ErrorCategory::Lune, -1, "unsupported message type")) break;
                continue;
            }
            if (lua.IsFaulted())
            {
                const auto error = lua.GetLastError();
                if (!pipe.SendError(error.category, error.line, error.message.c_str())) break;
                continue;
            }
            ScopedMonoAttach commandAttach(true);
            if (!commandAttach.IsAttached())
            {
                if (!pipe.SendError(protocol::ErrorCategory::Mono, -1, "failed to attach command thread to Mono"))
                    break;
                continue;
            }
            const std::string value(payload.begin(), payload.end());
            bool success;
            if (type == protocol::MSG_FILE && value.find('\0') != std::string::npos)
            {
                if (!pipe.SendError(protocol::ErrorCategory::Lua, -1, "file path contains a null byte")) break;
                continue;
            }
            success = type == protocol::MSG_CMD ? lua.ExecuteString(value.data(), value.size())
                                                : lua.ExecuteFile(value.c_str());
            if (lua.IsFaulted())
            {
                const auto error = lua.GetLastError();
                if (!pipe.SendError(error.category, error.line, error.message.c_str())) break;
                continue;
            }
            bool sent;
            if (success)
                sent = pipe.SendOk();
            else
            {
                const auto error = lua.GetLastError();
                sent = pipe.SendError(error.category, error.line, error.message.c_str());
            }
            if (!sent) break;
            MonoHook::DrainDeferred();
        }
        catch (const std::exception& exception)
        {
            char message[512]{};
            sprintf_s(message, "tool operation failed: %s",
                      exception.what() ? exception.what() : "unknown C++ exception");
            if (!pipe.SendError(protocol::ErrorCategory::Mono, -1, message)) break;
        }
        catch (...)
        {
            if (!pipe.SendError(protocol::ErrorCategory::Mono, -1, "tool operation failed")) break;
        }
    }
    if (bridge_lifecycle::g_processTerminating.load()) return;
    if (!frontendRequestedExit && pipe.IsConnected())
    {
        // 本地命令/日志/I/O 故障不能走正常卸载路径；保持模块映射，
        // 继续等待前端明确退出或对端真正关闭管道。
        g_unloadBlocked.store(true);
        WaitForFrontendExit();
        pipe.Shutdown();
        return;
    }
    ShutdownBridge();
}

static void WaitForFrontendExit() noexcept
{
    auto& pipe = PipeChannel::Instance();
    while (!bridge_lifecycle::g_processTerminating.load() && pipe.IsConnected())
    {
        try
        {
            uint8_t type = 0;
            std::vector<uint8_t> payload;
            if (!pipe.RecvFrame(type, payload))
            {
                if (pipe.IsConnected())
                {
                    Sleep(50);
                    continue;
                }
                break;
            }
            if (type == protocol::MSG_EXIT)
            {
                pipe.SendExit();
                break;
            }
            if (!pipe.SendError(protocol::ErrorCategory::Mono, -1,
                                "MonoLua session quarantined after a native fault; command rejected"))
                break;
        }
        catch (...)
        {
            // 故障隔离期间不再触碰 Mono 或 Lua；继续等待前端关闭连接。
            Sleep(50);
        }
    }
}

static void WorkerMainSehOnly()
{
    __try
    {
        WorkerMain();
    }
    __except (WorkerExceptionFilter(GetExceptionInformation()))
    {
        // 原生故障后不能再访问可能处于锁中间态的管道对象；只驻留，
        // 由过滤器已经写入调试输出，进程结束时由系统回收全部资源。
        QuarantineWorker();
    }
}

static void WorkerMainCppProtected() noexcept
{
    try
    {
        WorkerMainSehOnly();
    }
    catch (const std::exception& exception)
    {
        // 工作线程内部异常不允许触发 DLL 自卸载；即使管道已经不可用，
        // 也保留模块映射，直到游戏进程结束。
        g_unloadBlocked.store(true);
        OutputDebugStringA("[MonoLua] worker C++ exception: ");
        OutputDebugStringA(exception.what() ? exception.what() : "unknown C++ exception");
        OutputDebugStringA("\n");
        RecoverAfterWorkerException();
    }
    catch (...)
    {
        g_unloadBlocked.store(true);
        OutputDebugStringA("[MonoLua] worker unknown C++ exception\n");
        RecoverAfterWorkerException();
    }
}


void BridgeSession::Run() noexcept { WorkerMainCppProtected(); }
bool BridgeSession::CanUnload() noexcept
{
    return !bridge_lifecycle::g_processTerminating.load() &&
        !LuaEngine::Instance().IsFaulted() && !g_unloadBlocked.load();
}
