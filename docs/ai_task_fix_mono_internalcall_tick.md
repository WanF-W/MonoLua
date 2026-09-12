# MonoLua 优化任务：修复 Tick Hook 与主线程识别

## 发现的问题

在 Mono 游戏中：

```lua
local time = mono.get_class("UnityEngine", "Time")
local tick = time:get_method("get_deltaTime")
local ok, err = mono.set_tick(tick)
```

返回：

```text
false, failed to install native hook
```

普通 Mono 方法可以正常 Hook，因此不是 MinHook、权限或 Mono 初始化整体失效。

## 根因

`UnityEngine.Time.get_deltaTime` 是 Unity 的 native binding/InternalCall 方法，不是普通 C# 方法。

当前代码在 [mono_resolver.cpp](../src/mono_resolver.cpp) 中统一使用：

```cpp
mono_compile_method(method)
```

这个地址是 Mono 的托管调用/JIT wrapper 入口，不一定是 Unity 注册的真实 native 实现。该 wrapper 可能过短或不可重定位，导致 MinHook 安装失败。

当前代码还存在两个问题：

1. `mono_method_get_flags(method, nullptr)` 丢弃了方法实现标志 `iflags`，无法识别 InternalCall；
2. 没有调用 `mono_lookup_internal_call(method)` 获取 InternalCall 对应的 native 函数地址。

## 解决方案一：正确 Hook Unity InternalCall

修改以下文件：

```text
src/mono_metadata.h
src/mono_resolver.h
src/mono_resolver.cpp
src/mono_hook.cpp
```

### 1. 获取方法实现标志

增加接口：

```cpp
uint32_t MethodImplementationFlags(MonoMethod* method) const;
```

实现时必须传入 `&iflags`：

```cpp
uint32_t iflags = 0;
m_methodGetFlags(method, &iflags);
return iflags;
```

增加常量：

```cpp
constexpr uint32_t METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL = 0x1000;
```

### 2. 解析 InternalCall 地址

增加并解析：

```cpp
using FnLookupInternalCall = void* (*)(MonoMethod*);
```

提供：

```cpp
void* LookupInternalCall(MonoMethod* method) const;
```

### 3. 修改 Hook 目标选择

在 `PrepareHook()` 或统一的目标解析函数中使用以下逻辑：

```cpp
if (MethodImplementationFlags(method) & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
{
    target = LookupInternalCall(method);
    if (!target)
    {
        error = "internal call target is unavailable";
        return false;
    }
}
else
{
    target = CompileMethod(method);
    if (!target)
    {
        error = "failed to compile method";
        return false;
    }
}
```

普通方法继续使用 `mono_compile_method()`；InternalCall 方法优先使用 `mono_lookup_internal_call()`。

第一阶段至少确保以下方法可用：

```text
UnityEngine.Time.get_deltaTime
UnityEngine.Time.get_frameCount
```

不要删除 `get_deltaTime` 的默认 tick 候选，也不要要求用户改用其他游戏方法。

### 4. 保留 MinHook 详细错误

修改 `EnableNativeHook()`，不要再统一返回 `failed to install native hook`，应输出：

```cpp
error = "native hook failed: create=";
error += MH_StatusToString(created);
error += ", enable=";
error += MH_StatusToString(enabled);
```

## 解决方案二：增加独立的主线程探针

### 发现的问题

当前 `MonoScheduler::OnTick()` 在第一次进入 tick 时直接记录当前线程：

```cpp
if (!g_mainThread) g_mainThread = GetCurrentThreadId();
```

如果控制台线程通过 `runtime_invoke` 或 Lua 调用提前触发 tick，就会把控制台线程错误地认作 Unity 主线程，之后真正的主线程任务不会执行。

另外，当前 `MonoScheduler::SetTick()` 每次切换 tick 都会重置 `g_mainThread`，会破坏已经确认的主线程身份。

### 修改要求

1. 增加一次性的独立主线程探针，不要用任意 tick 的第一次调用来猜测主线程。
2. 探针候选按顺序尝试：

   ```text
   UnityEngine.UnitySynchronizationContext.ExecuteTasks
   UnityEngine.Time.get_deltaTime
   ```

3. 探针只在非嵌套的原生 Hook 入口确认线程。以下情况不能绑定主线程：

   - 控制台 `runtime_invoke` 调用；
   - Lua 回调中嵌套触发的方法调用；
   - 其他嵌套 Hook 分发。

4. 使用原子 CAS 只绑定一次有效线程身份：

   ```cpp
   DWORD expected = 0;
   g_mainThread.compare_exchange_strong(expected, GetCurrentThreadId());
   ```

5. `OnTick()` 只允许已确认的主线程排空队列：

   ```cpp
   if (g_mainThread == 0 || g_mainThread != GetCurrentThreadId())
       return;
   ```

6. 主线程探针确认成功后，立即取消探针身份；如果该方法没有同时作为 tick 或用户 Hook，则禁用其底层 Hook。
7. 探针、tick 和用户 Hook 使用独立状态，可以安全共存；同一 Method/同一 native 地址不能重复创建 MinHook。
8. `SetTick()` 切换 tick 时不要清空或重置已经确认的主线程 ID。
9. 探针或 tick 安装失败时，已成功入队的任务必须保留，后续重新设置 tick 时可以重试。
10. `is_tick_ready()` 只表示 tick Hook 已安装，不表示主线程探针已经确认执行线程。

### 建议接口和状态

可在 `MonoHook` 中增加类似接口：

```cpp
bool InstallMainThreadProbe(MonoMethod* method, void (*callback)(), std::string& error);
```

Hook 条目增加独立状态，例如：

```cpp
bool isMainThreadProbe = false;
void (*tickCallback)() = nullptr;
```

探针回调只负责确认线程，不执行 Lua 调度队列。调度队列仍由正式 tick 回调排空。

## 修改范围

至少检查并修改：

```text
src/mono_metadata.h
src/mono_resolver.h
src/mono_resolver.cpp
src/mono_hook.h
src/mono_hook.cpp
src/mono_scheduler.h
src/mono_scheduler.cpp
```

必要时同步更新：

```text
src/dll_main.cpp
README.md
```

## 注意事项

- 不要把所有 InternalCall 都无条件当成普通托管方法处理；不确定 ABI 的 InternalCall 应返回明确错误。
- 检查不同 Mono 方法是否共享同一个 native 地址，避免重复创建 MinHook。
- 不要使用 Unity 固定偏移。
- 不要修改 Lua API。
- 不要修改普通 Mono 方法的 Hook 行为。
- 不要把 tick 第一次被调用的线程直接当作主线程。
- 不要因 `set_tick()` 切换而重置已确认的主线程 ID。
- 不要让控制台调用或嵌套 Hook 触发主线程确认。

## 验收

在目标游戏中确认：

```lua
local time = mono.get_class("UnityEngine", "Time")
local tick = time:get_method("get_deltaTime")
local ok, err = mono.set_tick(tick)
assert(ok, err)

mono.schedule(function()
    print("scheduler callback executed")
end)
```

要求：

- `mono.set_tick(get_deltaTime)` 返回 `true`；
- 默认启动不再报告 scheduler unavailable；
- `mono.schedule()` 的回调实际执行；
- 控制台线程提前调用 tick 不会被登记为主线程；
- 主线程探针确认后，任务只在确认的主线程执行；
- 切换到普通 tick 方法不会清空已确认的主线程身份；
- 普通方法 Hook、`get_frameCount`、重复设置和退出清理没有回归。
