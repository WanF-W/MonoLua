/**
 * ============================================================
 * mono_runtime.h — Mono 高层运行时语义层
 * ============================================================
 * 在 Resolver 的薄函数包装之上提供线程安全的程序集缓存、名称归一化、
 * 查找、当前 Domain、metadata generation 和状态输出。程序集快照发生
 * 失效变化时递增 generation，使旧 Lua 元数据 userdata 无法继续访问。
 * ============================================================
 */
#pragma once

#include "common.h"
#include "mono_api.h"

struct MonoAssemblyInfo
{
    // 元数据指针由 Mono 持有，仅在所属运行时代次内有效。
    MonoAssembly* assembly = nullptr;
    MonoImage* image = nullptr;
    std::string name;
    uint64_t generation = 0;
};

class MonoRuntime
{
public:
    static MonoRuntime& Instance();
    bool Init();
    void Shutdown();
    bool RefreshAssemblies();
    bool IsInitialized() const { return m_initialized; }
    MonoDomain* Domain() const;
    uint64_t Generation() const { return m_generation.load(); }
    std::vector<MonoAssemblyInfo> Assemblies() const;
    bool FindAssembly(const std::string& name, MonoAssemblyInfo& result) const;
    bool FindAssemblyByImage(MonoImage* image, MonoAssemblyInfo& result) const;
    MonoClass* FindClass(const char* nameSpace, const char* name) const;
    bool FindUnityObjects(
        MonoClass* klass, std::vector<uint32_t>& handles, std::string& error) const;
    std::string Status() const;
    const std::string& LastError() const { return m_lastError; }
    const char* LastStage() const { return m_lastStage; }

private:
    MonoRuntime() = default;
    static void CollectAssembly(MonoAssembly* assembly, void* userData);
    static std::string NormalizeAssemblyName(std::string name);

    mutable std::mutex m_mutex;
    std::vector<MonoAssemblyInfo> m_assemblies;
    std::string m_lastError;
    const char* m_lastStage = "Mono runtime initialization has not started";
    MonoThread* m_workerThread = nullptr;
    MonoDomain* m_domain = nullptr;
    std::atomic<uint64_t> m_generation{1};
    bool m_initialized = false;
};
