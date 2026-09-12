# MonoLua 2.0.1 Release Notes

## 版本信息

- MonoLua 版本：`2.0.1`
- 协议版本：`MonoLua/2.0.1`
- 配套组件：MonoLua.dll `2.0.1` 与 Lune Mono 后端 `2.0.1`
- 变更范围：2.0.0 之后的 Hook、主线程调度、状态输出和文档维护更新。

<a name="version-and-pairing"></a>
## 1. 版本标识与配套要求

- [src/version.h](../src/version.h) 的 DLL 文件版本、产品版本和协议版本统一更新为 `2.0.1`。
- Lune 的 Mono 后端配置和握手字符串同步为 `MonoLua/2.0.1`；使用该 DLL 时必须配套相同协议版本的 Lune Mono 后端。
- Lune 自身产品版本独立于 MonoLua 后端版本，其他后端的版本配置不随本次更新改变。

<a name="lua-api-and-userdata"></a>
## 2. Lua 入口与 userdata 模型

- 不新增或删除 Lua 入口；现有 `mono.schedule`、`mono.set_tick`、`mono.get_tick` 和 `mono.is_tick_ready` 的调用形式保持不变。
- `mono.get_status()` 的字段与 Il2Cpp 状态输出靠拢，包含 `Initialized`、`Exports`、`Assemblies`、`Images`、`Main thread` 和日志拒绝计数。
- `mono.is_tick_ready()` 仅表示 tick Hook 已安装，不代表独立主线程探针已经确认执行线程。

<a name="assembly-and-class"></a>
## 3. 程序集与类型元数据

- Mono 状态统计增加有效 `Image` 数量；程序集、镜像、Root domain、Worker domain 和 metadata generation 仍来自当前运行时快照。
- 方法实现标志通过 `mono_method_get_flags(method, &iflags)` 读取，使 Hook 安装能够识别 `InternalCall` 方法。

<a name="method-and-field"></a>
## 4. 方法调用与字段读写

- 普通 Mono 方法继续使用 `mono_compile_method` 获取 JIT Hook 入口，原有参数和返回值限制不变。
- `InternalCall` 方法通过 `mono_lookup_internal_call` 解析真实 native 入口，避免把托管 wrapper 当作 Unity 原生实现。
- 当前明确支持 `UnityEngine.Time` 的静态、无参数、数值返回 getter，包括 `get_deltaTime`、`get_frameCount`、`get_timeScale`、`get_unscaledDeltaTime`、`get_realtimeSinceStartup` 和 `get_time`。
- 带对象、结构体、参数或其他不明确 native ABI 的 `InternalCall` 会在安装前返回明确错误；缺少可选解析导出时也不会伪造 Hook 地址。
- Hook 安装失败会报告 MinHook 的创建和启用状态；不同 Mono 方法共享同一 native 地址时拒绝重复注册。

<a name="containers-and-object-creation"></a>
## 5. 对象创建、数组与 List<T>

- 本版本没有改变对象创建、数组、List<T> 或值类型转换接口。

<a name="unity-query"></a>
## 6. Unity 对象查询与主线程调度

- 主线程身份由独立的一次性探针确认，候选顺序为 `UnityEngine.UnitySynchronizationContext.ExecuteTasks` 和 `UnityEngine.Time.get_deltaTime`。
- 探针只在非嵌套的原生 Hook 入口执行，并使用原子 CAS 绑定一次线程；控制台 Lua、`runtime_invoke` 和嵌套 Hook 不会登记线程。
- 探针确认后立即撤销探针回调；若入口没有同时承担 tick 或用户 Hook，则停用其底层 Hook。
- tick、探针和用户 Hook 使用独立状态，同一方法或 native 地址不会重复创建 MinHook。切换 tick 不会清空已确认的主线程 ID。
- `OnTick()` 只在已确认线程的非嵌套入口排空队列；探针或 tick 安装失败时已入队回调保留，可在之后重新设置 tick。
- Unity 对象查询接口本身没有改变；需要在 Unity 主线程执行的查询仍应通过 `mono.schedule()` 调用。

<a name="hooks"></a>
## 7. Mono JIT Hook

- Hook 条目增加独立的主线程探针回调状态，可与用户 Lua 回调和调度 tick 共存。
- 原生 Hook 分发记录嵌套深度，并在工具发起的托管调用期间禁止主线程探针和 tick 调度，避免控制台线程被误认或重入排队。
- Hook 的停用、元数据失效和退出清理会同时处理探针状态；已有用户 Hook 的行为保持不变。

<a name="runtime-lifecycle"></a>
## 8. Runtime、元数据与生命周期

- `runtime_invoke` 和 Lua 执行入口增加线程局部的工具调用作用域，用于区分主动调用与游戏自然进入的原生 Hook。
- 元数据刷新时清理探针、tick 和已失效的主线程身份；正常切换 tick 不触碰已确认线程。
- Resolver 新增的 `mono_lookup_internal_call` 按可选导出处理，并在状态查询中继续由 `mono.get_missing_exports()` 报告缺失项。

<a name="protocol-errors-and-io"></a>
## 9. 协议、错误与输出

- 协议帧结构没有改变，仅将 MonoLua 与 Lune Mono 后端的握手标识更新为 `MonoLua/2.0.1`。
- `mono.get_status()` 使用 `Rejected log batches` 标签；Mono 当前只统计提交阶段被拒绝的日志批次，不把发送失败或关闭时清空的分片混入该计数。
- Hook 准备、MinHook 创建/启用和 InternalCall 地址不可用时返回更具体的错误文本，同时保留旧错误前缀的兼容性。

<a name="project-and-documentation"></a>
## 10. 工程与使用文档

- 更新 MonoLua README 中的 tick 线程限制、InternalCall 支持范围、状态字段和日志计数语义。
- 更新 Lune README 中 Mono 后端的握手版本；Lune 的 Il2Cpp 既有版本配置保持不变。
- 新增本版本 ReleaseNote；发布平台使用的简短说明保存在 `.release_local/ReleaseOutline_2.0.1.md`，不提交到 Git。
