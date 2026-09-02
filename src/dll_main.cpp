/**
 * ============================================================
 * dll_main.cpp — MonoLua DLL 入口与模块生命周期
 * ============================================================
 * DllMain 只保存模块句柄并创建工作线程，避免在 Loader Lock 中执行 Mono、
 * Lua 或命名管道初始化。实际流程全部由 WorkerMain 完成：
 *
 * Pipe → HELLO → Mono Runtime → Lua → READY → 命令循环 → 逆序清理
 * ============================================================
 */
#include "common.h"
#include "lua_engine.h"
#include "mono_hook.h"
#include "mono_runtime.h"
#include "mono_scheduler.h"
#include "pipe_channel.h"

static HMODULE g_module = nullptr;
static HANDLE g_workerThread = nullptr;

static void WorkerMain()
{
    // IPC 必须最先初始化，使后续 Runtime/Lua 错误可以回传给 MLune。
    auto& pipe = PipeChannel::Instance();
    if (!pipe.Init()) return;
    if (!pipe.SendHello()) { pipe.Shutdown(); return; }

    // Mono Runtime 在 Lua 之前初始化，因为桥接全局表注册完成后就允许脚本
    // 访问 mono.get_assemblies 等 API。
    auto& runtime = MonoRuntime::Instance();

    // 游戏启动早期 Mono 模块或 Root Domain 可能尚未就绪，因此在工作
    // 线程中最多重试 30 秒，不在 DllMain 的 Loader Lock 内等待。
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
        std::string error = "failed to initialize Mono runtime: ";
        error += runtime.LastError().empty() ? "unknown error" : runtime.LastError();
        pipe.SendError(error.c_str());
        pipe.Shutdown();
        return;
    }

    // LuaEngine 只接收抽象输出回调，不直接依赖 IPC 单例。
    auto& lua = LuaEngine::Instance();
    if (!lua.Init(
            [&pipe](const char* text)
            {
                pipe.SendLog(text ? text : "");
            }))
    {
        pipe.SendError("failed to initialize Lua 5.4");
        runtime.Shutdown();
        pipe.Shutdown();
        return;
    }

    // READY 只表达启动链已经完成。详细的导出解析、程序集数量、Root
    // Domain 和模块基址由 mono.get_status() 按需查询，避免每次启动刷屏。
    pipe.SendReady("ready");
    // 命令在 DLL 工作线程串行执行。主线程调度器只负责投递任务，不会
    // 改变管道命令响应的一问一答关系。
    for (;;)
    {
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        if (!pipe.RecvFrame(type, payload)) break;
        const std::string value(payload.begin(), payload.end());
        switch (type)
        {
        case protocol::MSG_CMD:
            lua.ExecuteString(value.c_str())
                ? pipe.SendOk()
                : pipe.SendError("execution failed");
            break;

        case protocol::MSG_FILE:
            lua.ExecuteFile(value.c_str())
                ? pipe.SendOk()
                : pipe.SendError("file execution failed");
            break;

        case protocol::MSG_EXIT:
            pipe.SendExit();
            goto exit_loop;

        default:
            break;
        }
    }
exit_loop:

    // Hook 先禁止新回调并等待在途分发结束；调度器随后释放 Lua registry
    // 引用，Lua VM 关闭时回收 Instance handle，最后才清空 Mono 导出。
    MonoHook::Shutdown();
    MonoScheduler::Shutdown(lua.GetState());
    lua.Shutdown();
    runtime.Shutdown();
    pipe.Shutdown();
}

static DWORD WINAPI WorkerThread(void* parameter)
{
    (void)parameter;
    // SEH 只包裹一个不含局部 C++ 对象的入口，实际 RAII 对象都位于
    // WorkerMain 中，避免 MSVC /EHsc 与 __try 混用限制。
    __try { WorkerMain(); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // LastStage 只指向静态字符串，不在 SEH 处理块中构造 C++ 对象。
        // 这样即使 Mono 导出内部发生访问异常，MLune 也能看到准确边界。
        PipeChannel::Instance().SendError(MonoRuntime::Instance().LastStage());
        MonoHook::Shutdown();
        MonoScheduler::Shutdown(LuaEngine::Instance().GetState());
        LuaEngine::Instance().Shutdown();
        MonoRuntime::Instance().Shutdown();
        PipeChannel::Instance().Shutdown();
    }
    if (g_module != nullptr) FreeLibraryAndExitThread(g_module, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        // 关闭线程通知可减少游戏创建大量线程时对本 DLL 的无效回调。
        g_workerThread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (g_workerThread == nullptr) return FALSE;
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_workerThread != nullptr)
        {
            CloseHandle(g_workerThread);
            g_workerThread = nullptr;
        }
    }
    return TRUE;
}
