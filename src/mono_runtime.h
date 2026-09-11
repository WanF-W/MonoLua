/**
 * mono_runtime.h — Mono 高层运行时语义层
 * 在 Resolver 的薄函数包装之上提供线程安全的程序集缓存、名称归一化、
 * 查找、当前 Domain、metadata generation 和状态输出。程序集快照发生
 * 失效变化时递增 generation，使旧 Lua 元数据 userdata 无法继续访问。
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
    void DetachInitializationThread();
    bool HasInitializationThread() const { return m_workerThread != nullptr; }
    bool RefreshAssemblies();
    bool IsInitialized() const { return m_initialized.load(); }
    MonoDomain* Domain() const;
    uint64_t Generation() const { return m_generation.load(); }
    std::vector<MonoAssemblyInfo> Assemblies() const;
    bool FindAssembly(const std::string& name, MonoAssemblyInfo& result) const;
    bool FindAssemblyByImage(MonoImage* image, MonoAssemblyInfo& result) const;
    MonoClass* FindClass(const char* nameSpace, const char* name) const;
    bool InspectMethodGenerics(MonoMethod* method, bool& isGeneric) const;
    bool InspectOpenMethod(MonoMethod* method, bool& containsGenericParameters) const;
    bool TakeMetadataChanged();
    std::string Status() const;
    const std::string& LastError() const { return m_lastError; }

  private:
    MonoRuntime() = default;
    static void CollectAssembly(MonoAssembly* assembly, void* userData);
    static std::string NormalizeAssemblyName(std::string name);

    mutable std::mutex m_mutex;
    mutable std::map<MonoMethod*, bool> m_openMethodCache;
    mutable uint64_t m_openMethodGeneration = 0;
    std::vector<MonoAssemblyInfo> m_assemblies;
    std::string m_lastError;
    MonoThread* m_workerThread = nullptr;
    std::atomic<MonoDomain*> m_workerDomain{nullptr};
    std::atomic<uint64_t> m_generation{1};
    std::atomic<bool> m_initialized{false};
    bool m_metadataChanged = false;
};
