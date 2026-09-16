#include "common.h"
#include "bridge_session.h"

static HMODULE g_selfModule = nullptr;

static DWORD WINAPI WorkerThread(void*)
{
    BridgeSession::Run();
    // 正常关闭已经禁用 Hook，并等待所有原生回调返回。原生故障隔离时必须
    // 保留代码映射，直到进程结束。
    if (BridgeSession::CanUnload())
        FreeLibraryAndExitThread(g_selfModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    static_cast<void>(reserved);
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_selfModule = module;
        HANDLE worker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (!worker)
        {
            // 工作线程创建失败时保留已加载模块，避免加载器回滚并把注入
            // 误报成进程级失败；当前实例保持惰性驻留，等待进程结束回收。
            OutputDebugStringA("[MonoLua] failed to create worker thread; DLL retained\n");
            return TRUE;
        }
        CloseHandle(worker);
    }
    else if (reason == DLL_PROCESS_DETACH)
        bridge_lifecycle::g_processTerminating.store(true);
    return TRUE;
}
