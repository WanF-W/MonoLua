# MonoLua 3.0.1 Release Notes

## 版本信息

- MonoLua 版本：`3.0.1`
- 协议版本：`MonoLua/3.0.1`
- 配套组件：MonoLua.dll `3.0.1`、HostCore Mono 后端 `3.0.1`、Lune Mono 握手配置 `MonoLua/3.0.1`
- 变更范围：移除独立 Unity 主线程探针，改进 Mono JIT Hook 的 MinHook 跳板分配，并修正启动日志排版。

<a name="version-and-pairing"></a>
## 1. 版本标识与配套要求

- [src/version.h](../src/version.h) 将 DLL 文件版本、产品版本和协议版本统一更新为 `3.0.1`，协议字符串为 `MonoLua/3.0.1`。
- Lune 的 Mono profile 和握手说明同步为 `MonoLua/3.0.1`；使用该 DLL 时必须配套相同握手字符串的 HostCore/Lune Mono 后端。
- 二进制帧格式、消息类型和负载限制没有改变。版本不一致时 HELLO 校验仍会终止会话，不会继续进入 READY 或 REPL。

<a name="unity-query-and-scheduling"></a>
## 2. Unity 查询与主线程调度

- 默认调度入口现在只有 `UnityEngine.Time.get_deltaTime`。独立安装 `UnitySynchronizationContext.ExecuteTasks` 的一次性主线程探针已移除，`get_frameCount` 和 `Object.get_name` 也不再作为自动后备入口。
- 移除探针的原因是它只负责确认一次线程身份，却额外创建一个 Mono JIT Hook；实际调度仍需要一个会自然执行的 tick。对 Unity 游戏而言，`get_deltaTime` 已经是稳定且高频的主线程入口，直接让它的首次自然调用记录线程即可，减少一次 Hook 和启动阶段的偶发错误。
- 线程身份在 tick 的首次自然、非嵌套调用中记录；控制台 Lua 调用、MonoLua 发起的 `runtime_invoke` 和嵌套 Hook 不参与记录。若游戏在工作线程直接调用该入口，工作线程可能先被记录；这是该方案依赖运行时调用行为的限制。
- `mono.schedule()`、`mono.set_tick()`、`mono.is_tick_ready()` 的公开入口保持不变。调度器安装失败时基础会话仍可完成握手，排队任务可在后续成功设置 tick 后继续处理。

<a name="hooks"></a>
## 3. Hook 行为与 MinHook 分配

- 调查 `MH_ERROR_MEMORY_ALLOC` 后确认，失败发生在 MinHook 为 Mono JIT 入口寻找附近 trampoline 区域的阶段；它不是 `ExecuteTasks` 的方法签名或 Unity 语义限制。x64 MinHook 默认只在目标附近有限范围内搜索，Mono JIT 代码地址和游戏运行时的内存碎片会使该范围内没有可用的按粒度对齐区域，因而出现偶发失败。类似的 Mono JIT 与 `MH_ERROR_MEMORY_ALLOC` 情况也见于 [MinHook Issue #107](https://github.com/TsudaKageyu/minhook/issues/107)。
- 保留 MinHook，并在原有附近搜索失败后扩大可用搜索范围；跳板生成同时检查 RIP 相对寻址和入口 `rel32` 跳转的位移是否仍在合法范围内，超限时安全返回失败，不截断地址。
- 普通游戏方法 Hook 的 API、回调格式、原方法调用和 ABI 限制没有改变。扩大搜索范围提高成功率，但无法保证任意进程地址布局下所有 Hook 都能安装。

<a name="protocol-and-output"></a>
## 4. 协议、错误与输出

- READY 握手语义保持不变。调度器失败诊断仍通过日志帧发送，但 Lune 在启动握手期间暂存后台日志，待 `[+] Ready` 完整输出后再显示，避免诊断文本与 Ready 字符交错。
- 调度器诊断不再包含 `ExecuteTasks` 探针失败信息；如果默认 `Time.get_deltaTime` 也无法安装，会报告该入口的安装错误，基础会话仍可通信。

<a name="documentation-and-project"></a>
## 5. 文档与工程调整

- README 更新为默认 `get_deltaTime` 调度和首次自然 tick 线程记录的实际行为，并移除独立探针和自动后备入口的说明。
- 更新 MonoLua 与 Lune 的版本配对信息，新增本版本 ReleaseNote；Lune 自身产品版本以及其他后端的版本配置保持独立。
