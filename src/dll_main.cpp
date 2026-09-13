// DLL composition root: Lune IPC -> Mono -> Lua -> command loop -> orderly shutdown.
#include "common.h"
#include "lua_engine.h"
#include "mono_hook.h"
#include "mono_runtime.h"
#include "mono_resolver.h"
#include "mono_scheduler.h"
#include "pipe_channel.h"
#include <cstdio>

static HMODULE g_selfModule = nullptr;

static int WorkerExceptionFilter(EXCEPTION_POINTERS* info) noexcept
{
    if (info && info->ExceptionRecord)
    {
        char message[160]{};
        sprintf_s(message, "[MonoLua] worker native exception 0x%08lX at %p; session quarantined\n",
                  info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
        OutputDebugStringA(message);
    }
    return LuaEngine::Instance().HandleNativeFault();
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
    // After a native fault, locks or VM frames may be damaged. Keep runtime and hook
    // resources resident; later hook entries bypass Lua. The OS reclaims them at exit.
    if (!LuaEngine::Instance().IsFaulted())
    {
        {
            auto& runtime = MonoRuntime::Instance();
            ScopedMonoAttach cleanupAttach(runtime.IsInitialized() && !runtime.HasInitializationThread());
            MonoHook::Shutdown();
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
    if (!pipe.Init()) return;
    if (!pipe.SendHello())
    {
        pipe.Shutdown();
        return;
    }
    auto& runtime = MonoRuntime::Instance();
    bool runtimeReady = false;
    for (int retry = 0; retry < 60; ++retry)
    {
        if (runtime.Init())
        {
            runtimeReady = true;
            break;
        }
        Sleep(500);
    }
    if (!runtimeReady)
    {
        const std::string error = "failed to initialize Mono runtime: " + runtime.LastError();
        pipe.SendError(protocol::ErrorCategory::Mono, -1, error.c_str());
        pipe.Shutdown();
        return;
    }

    auto& lua = LuaEngine::Instance();
    if (!lua.Init([&pipe](const char* text) { pipe.SendLog(text); },
                  [] { PipeChannel::Instance().Disconnect(); }))
    {
        pipe.SendError(protocol::ErrorCategory::Lua, -1, "failed to initialize Lua 5.4");
        ShutdownBridge();
        return;
    }
    std::string tickError;
    MonoScheduler::AutoSetTick(tickError);
    // READY 只表示运行时和 Lua 已经可以通信；调度器诊断在握手完成后单独发送。
    if (!pipe.SendReady("ready"))
    {
        ShutdownBridge();
        return;
    }
    MonoScheduler::FlushDiagnostics();

    runtime.DetachInitializationThread();
    while (!lua.IsFaulted())
    {
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        if (!pipe.RecvFrame(type, payload)) break;
        if (type == protocol::MSG_EXIT)
        {
            pipe.SendExit();
            break;
        }
        if (type != protocol::MSG_CMD && type != protocol::MSG_FILE) continue;
        ScopedMonoAttach commandAttach(true);
        if (!commandAttach.IsAttached())
        {
            pipe.SendError(protocol::ErrorCategory::Mono, -1, "failed to attach command thread to Mono");
            break;
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
        if (lua.IsFaulted()) break;
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
    ShutdownBridge();
}

static DWORD WINAPI WorkerThread(void*)
{
    __try
    {
        WorkerMain();
    }
    __except (WorkerExceptionFilter(GetExceptionInformation()))
    {
        // Do not run Lua finalizers or Mono cleanup after an escaped native fault.
        PipeChannel::Instance().Shutdown();
    }
    // Shutdown has disabled hooks and waited for their complete native return paths.
    // A native-fault quarantine must keep its code mapped until the process exits.
    if (!LuaEngine::Instance().IsFaulted()) FreeLibraryAndExitThread(g_selfModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_selfModule = module;
        HANDLE worker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (!worker) return FALSE;
        CloseHandle(worker);
    }
    else if (reason == DLL_PROCESS_DETACH && reserved)
        bridge_lifecycle::g_processTerminating.store(true);
    return TRUE;
}
