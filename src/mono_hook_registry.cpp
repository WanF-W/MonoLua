#include "mono_hook.h"
#include "mono_hook_internal.h"
#include "hook_stub.h"
#include "lua_engine.h"
#include "mono_feature_fault.h"
extern "C" { extern volatile LONGLONG g_hookStubUsers; }
namespace mono_hook_detail
{
    constexpr size_t THUNK_SIZE = 19;
    std::mutex g_mutex;
    std::vector<std::unique_ptr<HookEntry>> g_entries;
    std::map<MonoMethod*, HookEntry*> g_methods;
    bool g_minHookReady = false;
    std::atomic<bool> g_shutdown{false};
    std::vector<int> g_deferredLuaRefs;
    bool g_metadataCleanupPending = false;
    HookEntry* g_tickEntry = nullptr;
    // 调用方持有 g_mutex。若在创建可执行资源前发生 Lua 分配或普通设置失败，
    // 撤销容器登记。
    struct PendingRegistration
    {
        HookEntry* entry;
        ~PendingRegistration()
        {
            if (bridge_lifecycle::g_sessionFaulted.load() || entry->enabled || entry->luaRef != LUA_REFNIL ||
                entry->tickCallback)
                return;
            if (entry->thunk)
            {
                const MH_STATUS removed = SafeMinHookCall(MH_RemoveHook, entry->target);
                if (removed != MH_OK && removed != MH_ERROR_NOT_CREATED) return;
                VirtualFree(entry->thunk, 0, MEM_RELEASE);
                entry->thunk = nullptr;
            }
            if (!g_entries.empty() && g_entries.back().get() == entry)
            {
                g_methods.erase(entry->method);
                g_entries.pop_back();
            }
        }
    };

    bool HookStubsActive()
    {
        return InterlockedCompareExchange64(const_cast<LONGLONG*>(&g_hookStubUsers), 0, 0) != 0;
    }

    bool WaitForHookStubs(DWORD timeout = 5000)
    {
        const ULONGLONG deadline = GetTickCount64() + timeout;
        while (HookStubsActive())
        {
            if (GetTickCount64() >= deadline) return false;
            Sleep(1);
        }
        return true;
    }

    void QueueLuaRefLocked(int reference)
    {
        if (reference != LUA_REFNIL) g_deferredLuaRefs.push_back(reference);
    }

    bool EnsureMinHook(std::string& error)
    {
        if (g_minHookReady) return true;
        const MH_STATUS status = SafeMinHookCall(MH_Initialize);
        g_minHookReady = status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
        if (!g_minHookReady) error = std::string("native hook initialization failed: ") + MH_StatusToString(status);
        return g_minHookReady;
    }

    void* AllocateThunk(uint32_t id)
    {
        auto* memory = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, THUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!memory) return nullptr;
        memory[0] = 0xB8;
        memcpy(memory + 1, &id, sizeof(id));
        memory[5] = 0xFF;
        memory[6] = 0x25;
        memset(memory + 7, 0, 4);
        void* detour = GetHookDetourAddress();
        memcpy(memory + 11, &detour, sizeof(detour));
        FlushInstructionCache(GetCurrentProcess(), memory, THUNK_SIZE);
        return memory;
    }

    bool EnableNativeHook(HookEntry& entry, std::string& error)
    {
        entry.thunk = AllocateThunk(entry.id);
        if (!entry.thunk)
        {
            error = "failed to allocate native thunk";
            return false;
        }
        const MH_STATUS created = SafeMinHookCall(MH_CreateHook, entry.target, entry.thunk, &entry.original);
        const MH_STATUS enabled = created == MH_OK ? SafeMinHookCall(MH_EnableHook, entry.target) : MH_UNKNOWN;
        if (created != MH_OK || enabled != MH_OK)
        {
            error = std::string("native hook failed: create=") + MH_StatusToString(created) +
                    ", enable=" + (created == MH_OK ? MH_StatusToString(enabled) : "not attempted") +
                    " (failed to install native hook)";
            if (created == MH_OK)
            {
                const MH_STATUS removed = SafeMinHookCall(MH_RemoveHook, entry.target);
                if (removed != MH_OK && removed != MH_ERROR_NOT_CREATED)
                {
                    // 管理器已经拥有该条目。如果 MinHook 仍可能引用这段代码，
                    // 保留可执行内存，退出时再重试清理。
                    entry.enabled = true;
                    error += std::string("; rollback remove failed: ") + MH_StatusToString(removed);
                    return false;
                }
            }
            VirtualFree(entry.thunk, 0, MEM_RELEASE);
            entry.thunk = nullptr;
            return false;
        }
        entry.enabled = true;
        return true;
    }

}
using namespace mono_hook_detail;

bool MonoHook::HookMethod(lua_State* state, MonoMethod* method, int callbackIndex, std::string& error)
{
    error.clear();
    if (g_shutdown.load())
    {
        error = "hook manager has already been shut down";
        return false;
    }
    DrainDeferred();
    if (!method || lua_type(state, callbackIndex) != LUA_TFUNCTION)
    {
        error = "hook requires a function";
        return false;
    }
    HookEntry prepared;
    if (!PrepareHook(method, prepared, error)) return false;
    const auto& parameters = prepared.parameters;
    MonoType* returnType = prepared.returnType;
    void* target = prepared.target;
    std::lock_guard<std::mutex> lock(g_mutex);
    // MinHook 的全局状态与本模块的 Hook 表必须使用同一串行化边界；否则
    // 命令线程和 Hook 回调线程同时注册方法时可能并发初始化 MinHook。
    if (!EnsureMinHook(error)) return false;
    if (g_metadataCleanupPending)
    {
        error = "metadata invalidation is still waiting for active hooks";
        return false;
    }
    auto existing = g_methods.find(method);
    if (existing != g_methods.end())
    {
        HookEntry* entry = existing->second;
        entry->parameters = parameters;
        entry->returnType = returnType;
        entry->isStatic = prepared.isStatic;
        lua_pushvalue(state, callbackIndex);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        const MH_STATUS status = SafeMinHookCall(MH_EnableHook, entry->target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
            error = std::string("failed to re-enable method hook: ") + MH_StatusToString(status);
            return false;
        }
        QueueLuaRefLocked(entry->luaRef);
        entry->luaRef = reference;
        entry->enabled = true;
        return true;
    }
    // 泛型共享代码可能让两个 MonoMethod 编译到同一原生入口。一个入口
    // 只能安装一个 thunk；不同 Method 的签名/声明类无法可靠区分，因此
    // 明确拒绝，避免 MinHook 返回含糊错误或把回调绑定到错误的方法。
    for (const auto& item : g_entries)
    {
        if (item && item->target == target)
        {
            error = "method shares its native address with another registered Mono method";
            return false;
        }
    }
    auto entry = std::make_unique<HookEntry>(std::move(prepared));
    entry->id = static_cast<uint32_t>(g_entries.size());
    lua_pushvalue(state, callbackIndex);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    entry->luaRef = reference;
    // Lua 引用成功后再登记条目。若 luaL_ref 因 Lua 分配失败抛出异常，
    // 不留下半登记的原生条目。
    g_entries.reserve(g_entries.size() + 1);
    HookEntry* next = entry.get();
    g_methods.emplace(method, next);
    g_entries.push_back(std::move(entry));
    PendingRegistration registration{next};
    if (!EnableNativeHook(*next, error))
    {
        luaL_unref(state, LUA_REGISTRYINDEX, next->luaRef);
        next->luaRef = LUA_REFNIL;
        return false;
    }
    return true;
}

bool MonoHook::UnhookMethod(MonoMethod* method)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto found = g_methods.find(method);
        if (found == g_methods.end()) return false;
        if (found->second->luaRef == LUA_REFNIL) return true;
        HookEntry* entry = found->second;
        QueueLuaRefLocked(entry->luaRef);
        entry->luaRef = LUA_REFNIL;
        if (!entry->tickCallback)
        {
            const MH_STATUS disabled = SafeMinHookCall(MH_DisableHook, entry->target);
            if (disabled == MH_OK || disabled == MH_ERROR_DISABLED) entry->enabled = false;
        }
    }
    // 若当前调用来自 Hook 回调，不能等待自身退出；此时只延迟 unref，
    // 下一个安全生命周期点会由 DrainDeferred() 完成回收。
    DrainDeferred();
    return true;
}

bool MonoHook::IsHooked(MonoMethod* method)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_methods.find(method);
    return found != g_methods.end() && found->second->enabled && found->second->luaRef != LUA_REFNIL;
}

static bool InstallInternalHook(MonoMethod* method, void (*callback)(), std::string& error,
                                const MonoScheduler::Candidate* candidate = nullptr)
{
    error.clear();
    if (!method || !callback)
    {
        error = "internal hook requires a method and callback";
        return false;
    }
    if (g_shutdown.load())
    {
        error = "hook manager has already been shut down";
        return false;
    }
    MonoHook::DrainDeferred();
    HookEntry prepared;
    if (!PrepareHook(method, prepared, error, candidate)) return false;
    void* target = prepared.target;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!EnsureMinHook(error)) return false;
    if (g_metadataCleanupPending)
    {
        error = "metadata invalidation is still waiting for active hooks";
        return false;
    }

    // 只有 Hook 启用成功后才提交选中的调度角色。
    const auto commitRole = [callback, &error](HookEntry* next) {
        HookEntry*& current = g_tickEntry;
        if (current && current != next)
        {
            if (current->luaRef == LUA_REFNIL)
            {
                const MH_STATUS disabled = SafeMinHookCall(MH_DisableHook, current->target);
                if (disabled != MH_OK && disabled != MH_ERROR_DISABLED)
                {
                    error = std::string("failed to replace scheduler hook: ") + MH_StatusToString(disabled);
                    if (next->luaRef == LUA_REFNIL && !next->tickCallback)
                    {
                        const MH_STATUS rollback = SafeMinHookCall(MH_DisableHook, next->target);
                        if (rollback == MH_OK || rollback == MH_ERROR_DISABLED) next->enabled = false;
                        else
                        {
                            error += std::string("; new hook disable failed: ") + MH_StatusToString(rollback);
                            // 不能把仍可能被 MinHook 使用的 thunk 留在普通登记表中，
                            // 后续安全点必须继续尝试禁用和移除。
                            g_metadataCleanupPending = true;
                        }
                    }
                    return false;
                }
                current->enabled = false;
            }
            current->tickCallback = nullptr;
        }
        next->tickCallback = callback;
        next->enabled = true;
        current = next;
        return true;
    };
    for (auto& item : g_entries)
    {
        if (!item || item->target != target) continue;
        if (item->method != method)
        {
            error = "internal hook method shares its native address with another registered Mono method";
            return false;
        }
        const MH_STATUS status = SafeMinHookCall(MH_EnableHook, target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            error = std::string("failed to enable internal hook: ") + MH_StatusToString(status);
            return false;
        }
        item->enabled = true;
        return commitRole(item.get());
    }
    if (g_methods.find(method) != g_methods.end())
    {
        error = "method native address changed; refresh metadata before reinstalling";
        return false;
    }
    auto entry = std::make_unique<HookEntry>(std::move(prepared));
    entry->id = static_cast<uint32_t>(g_entries.size());
    // 修改可执行代码前先完成所有拥有容器的分配。
    g_entries.reserve(g_entries.size() + 1);
    HookEntry* next = entry.get();
    g_methods.emplace(method, next);
    g_entries.push_back(std::move(entry));
    PendingRegistration registration{next};
    if (!EnableNativeHook(*next, error))
    {
        return false;
    }
    return commitRole(next);
}

bool MonoHook::InstallTick(MonoMethod* method, void (*callback)(), std::string& error)
{
    return InstallInternalHook(method, callback, error);
}

bool MonoHook::InstallSchedulerEntry(MonoMethod* method, const MonoScheduler::Candidate& candidate,
                                     void (*callback)(), std::string& error)
{
    // 调度器候选来自目标游戏的私有 Mono 构建。元数据由窄边界保护，
    // MinHook 也逐调用保护；这里不包住内部互斥锁，避免跳过锁析构。
    bridge_lifecycle::ClearNativeCallFault();
    const bool success = InstallInternalHook(method, callback, error, &candidate);
    if (success) return true;
    if (ConsumeNativeCallFault("scheduler hook installation", error)) return false;
    if (error.empty()) error = "scheduler hook installation failed";
    return false;
}

void MonoHook::UnhookAll()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry : g_entries)
        {
            if (!entry || entry->luaRef == LUA_REFNIL) continue;
            QueueLuaRefLocked(entry->luaRef);
            entry->luaRef = LUA_REFNIL;
            if (!entry->tickCallback)
            {
                const MH_STATUS disabled = SafeMinHookCall(MH_DisableHook, entry->target);
                if (disabled == MH_OK || disabled == MH_ERROR_DISABLED) entry->enabled = false;
            }
        }
    }
    DrainDeferred();
}

void MonoHook::InvalidateMetadata()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry : g_entries)
        {
            if (!entry) continue;
            bool disabled = true;
            if (entry->enabled)
            {
                const MH_STATUS status = SafeMinHookCall(MH_DisableHook, entry->target);
                disabled = status == MH_OK || status == MH_ERROR_DISABLED;
            }
            QueueLuaRefLocked(entry->luaRef);
            entry->luaRef = LUA_REFNIL;
            if (disabled) entry->enabled = false;
            entry->tickCallback = nullptr;
        }
        g_methods.clear();
        g_tickEntry = nullptr;
        g_metadataCleanupPending = true;
    }
    // 不能在仍有 Hook 回调时释放 trampoline 或 Lua 引用。若当前就在
    // Hook 内调用刷新，这里只标记待处理；后续命令结束或安装 Hook 时再次尝试回收。
    DrainDeferred();
}

void MonoHook::DrainDeferred()
{
    if (LuaEngine::Instance().IsFaulted()) return;
    if (HookStubsActive()) return;
    auto& engine = LuaEngine::Instance();
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    std::lock_guard<std::mutex> lock(g_mutex);
    if (HookStubsActive()) return; // 获取 Lua 锁后再次检查
    lua_State* state = engine.GetState();
    if (g_metadataCleanupPending)
    {
        bool allRemoved = true;
        for (auto& entry : g_entries)
        {
            if (!entry) continue;
            if (entry->enabled)
            {
                const MH_STATUS disabled = SafeMinHookCall(MH_DisableHook, entry->target);
                if (disabled != MH_OK && disabled != MH_ERROR_DISABLED)
                {
                    allRemoved = false;
                    continue;
                }
            }
            const MH_STATUS removed = SafeMinHookCall(MH_RemoveHook, entry->target);
            if (removed != MH_OK && removed != MH_ERROR_NOT_CREATED)
            {
                allRemoved = false;
            }
        }
        if (!allRemoved) return;
        for (auto& entry : g_entries)
        {
            if (!entry) continue;
            if (entry->thunk) VirtualFree(entry->thunk, 0, MEM_RELEASE);
        }
        g_entries.clear();
        g_methods.clear();
        g_tickEntry = nullptr;
        g_metadataCleanupPending = false;
    }
    for (int reference : g_deferredLuaRefs)
        if (state) luaL_unref(state, LUA_REGISTRYINDEX, reference);
    g_deferredLuaRefs.clear();
}

bool MonoHook::Shutdown()
{
    if (LuaEngine::Instance().IsFaulted()) return false;
    g_shutdown.store(true);
    bool disabled = true;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry : g_entries)
            if (entry && entry->enabled)
            {
                const MH_STATUS status = SafeMinHookCall(MH_DisableHook, entry->target);
                if (status != MH_OK && status != MH_ERROR_DISABLED)
                    disabled = false;
                else
                    entry->enabled = false;
            }
    }
    if (!disabled)
    {
        g_shutdown.store(false);
        return false;
    }

    // Disable 后仍可能有线程已经进入共享跳板，必须等整个跳板（包括
    // trampoline 调用和返回路径）退出，才能删除 MinHook trampoline。
    if (!WaitForHookStubs())
    {
        // 仍有游戏线程在跳板内运行时不能继续移除 trampoline；保留资源，
        // 由进程结束回收，避免卸载后跳入已释放代码。
        g_shutdown.store(false);
        return false;
    }
    if (LuaEngine::Instance().IsFaulted())
    {
        g_shutdown.store(false);
        return false;
    }

    auto& engine = LuaEngine::Instance();
    std::lock_guard<std::recursive_mutex> luaLock(engine.GetMutex());
    std::lock_guard<std::mutex> lock(g_mutex);
    lua_State* state = engine.GetState();
    bool removed = true;
    for (auto& entry : g_entries)
    {
        if (!entry) continue;
        const MH_STATUS status = SafeMinHookCall(MH_RemoveHook, entry->target);
        if (status != MH_OK && status != MH_ERROR_NOT_CREATED) removed = false;
    }
    if (!removed)
    {
        // MinHook 拒绝删除时保留所有 thunk 和运行时状态，禁止卸载 DLL，
        // 避免游戏线程稍后跳入已释放的代码。
        g_shutdown.store(false);
        return false;
    }
    for (auto& entry : g_entries)
    {
        if (!entry) continue;
        QueueLuaRefLocked(entry->luaRef);
        entry->luaRef = LUA_REFNIL;
        if (entry->thunk) VirtualFree(entry->thunk, 0, MEM_RELEASE);
    }
    g_entries.clear();
    g_methods.clear();
    g_tickEntry = nullptr;
    g_metadataCleanupPending = false;
    for (int reference : g_deferredLuaRefs)
        if (state) luaL_unref(state, LUA_REGISTRYINDEX, reference);
    g_deferredLuaRefs.clear();
    if (g_minHookReady)
    {
        const MH_STATUS status = SafeMinHookCall(MH_Uninitialize);
        if (status != MH_OK && status != MH_ERROR_NOT_INITIALIZED)
        {
            // 入口已经恢复，但 MinHook 的全局状态仍不确定；保留模块，
            // 以后在安全时机重试，禁止把清理失败伪装成正常卸载。
            g_shutdown.store(false);
            return false;
        }
        g_minHookReady = false;
    }
    g_shutdown.store(false);
    return true;
}
