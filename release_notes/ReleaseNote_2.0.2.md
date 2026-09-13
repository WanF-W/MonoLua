# MonoLua 2.0.2 Release Notes

## 版本信息

- MonoLua 版本：`2.0.2`
- 协议版本：`MonoLua/2.0.2`
- 配套组件：MonoLua.dll `2.0.2` 与 Lune Mono 后端 `2.0.2`
- 变更范围：主线程调度失败隔离、Unity 固定入口 Hook、泛型元数据检查和 Hook 生命周期安全性。

<a name="version-and-pairing"></a>
## 1. 版本标识与配套要求

- [src/version.h](../src/version.h) 的 DLL 文件版本、产品版本和协议版本统一更新为 `2.0.2`。
- HostCore 的 Mono 后端配置和 Lune README 中的握手说明同步为 `MonoLua/2.0.2`；使用该 DLL 时必须配套相同协议版本的 Lune Mono 后端。
- Lune 自身产品版本独立于 MonoLua 后端版本，其他后端的版本配置不随本次更新改变。
- 协议帧结构没有改变；HELLO 负载必须精确匹配 `MonoLua/2.0.2`。

<a name="lua-api-and-userdata"></a>
## 2. Lua 入口与 userdata 模型

- 不新增或删除 Lua 入口；`mono.schedule`、`mono.set_tick`、`mono.get_tick`、`mono.is_tick_ready` 和 `mono.get_status` 的调用形式保持不变。
- `mono.is_tick_ready()` 继续只表示 tick Hook 已安装，不代表独立主线程探针已经确认执行线程。
- 调度器成功安装候选入口时不向 Lune 输出成功日志；候选失败或调度器最终不可用时才输出诊断，避免正常握手被内部安装信息污染。

<a name="assembly-and-class"></a>
## 3. 程序集与类型元数据

- 调度器内置候选通过 `UnityEngine` 命名空间下的类名、方法名和无参数签名查找，不再要求程序集文件名精确匹配，也不读取与调用约定无关的实现标志。
- 固定候选的 ABI 描述集中在 `src/mono_scheduler_candidates.h`，分别定义 `ExecuteTasks`、`get_deltaTime`、`get_frameCount` 和 `Object.get_name` 的静态性及返回类型。
- 元数据查询发生访问异常时只返回带阶段信息的功能错误，不把单个候选失败升级为整个会话故障。

<a name="method-and-field"></a>
## 4. 方法调用与字段读写

- 普通 Mono 方法继续使用 `mono_compile_method` 获取 JIT Hook 入口，现有参数、返回值和 InternalCall ABI 限制保持不变。
- 调度器固定入口使用专用准备路径，直接校验参数、静态性、返回类型和支持的 ABI，不调用通用泛型反射检查。
- 通用用户 Hook 的泛型判断改为三态结果：确认非泛型时继续，确认泛型或泛型声明类型时拒绝，无法判断时只拒绝当前 Hook 并返回具体原因。
- 移除对不存在或不稳定的 `mono_method_is_generic`、`mono_method_is_inflated` 导出依赖；`mono.get_missing_exports()` 不再报告这两个名称。

<a name="containers-and-object-creation"></a>
## 5. 对象创建、数组与 List<T>

- 本版本没有改变对象创建、数组、List<T> 或值类型转换接口。

<a name="unity-query"></a>
## 6. Unity 对象查询与主线程调度

- DLL 初始化阶段仍自动尝试主线程调度；`Ready` 只表示运行时和 Lua 已完成初始化，调度器诊断在握手完成后单独发送。
- 主线程身份由一次性探针确认，候选顺序为 `UnitySynchronizationContext.ExecuteTasks`、`Time.get_deltaTime`；tick 候选顺序为 `Time.get_deltaTime`、`Time.get_frameCount`、`UnityEngine.Object.get_name`。
- 探针和 tick 候选逐个尝试。一个入口的 native Hook 创建或启用失败时继续后备入口；全部失败只报告调度器不可用，基础 Lua/管道会话仍保持可用。
- `mono.schedule()` 会先保存回调；调度器暂时不可用或尚未确认线程时，已入队任务继续保留，之后可通过 `mono.set_tick()` 或后续自动尝试恢复。
- 工具发起的 Lua 调用、`runtime_invoke` 和嵌套 Hook 不参与线程确认；确认后撤销一次性探针，并继续复用已经安装的 tick 或用户 Hook。
- `mono.set_tick()` 替换失败时保留已有 tick 和任务队列；切换 tick 不重置已确认的主线程身份。

<a name="hooks"></a>
## 7. Mono JIT Hook

- Hook 元数据准备、调度器候选发现和原生 Hook 安装都具有局部 native fault 边界；访问异常被转换为当前功能的错误文本，不调用会话级 `Abort`，也不关闭通信管道。
- Hook 注册改为事务式提交：Lua 引用、Hook 表和 MinHook 资源按可回滚顺序登记，创建或启用失败时清理半完成条目，避免留下失效 trampoline 或错误角色状态。
- 同一个 native 地址不能绑定到多个不同的 Mono 方法；tick、探针和用户 Hook 共用已有条目时只切换角色，不重复创建 MinHook。
- 替换调度角色时先确认旧 Hook 可以停用；停用失败会保留旧状态并回滚新角色，避免调度器切换破坏已有 Hook。
- 元数据失效和退出清理会等待在途 Hook stub 返回后再删除 trampoline；无法安全回收时保留资源到后续安全清理点。

<a name="runtime-lifecycle"></a>
## 8. Runtime、元数据与生命周期

- 泛型反射检查通过 `MethodInfo.IsGenericMethod` 和声明类型的 `Type.IsGenericType` 查询，查询过程使用 GC handle 和局部异常边界；反射结果不可用时返回 `Unknown`，不把普通方法误判为泛型。
- 调度器候选的发现、元数据读取和 MinHook 改写异常只影响调度器功能；只有未被局部边界拦截的工作线程 native fault 才进入会话隔离路径。
- 工作线程的 native fault 记录异常地址和阶段，并进入 Lua/管道故障隔离；故障路径不再继续执行不安全的 Lua finalizer 或 Mono 清理。
- 元数据刷新会撤销探针、tick 和已确认线程身份；队列引用在失效时清理，正常 tick 切换不触碰已确认线程。

<a name="protocol-errors-and-io"></a>
## 9. 协议、错误与输出

- HELLO 握手标识更新为 `MonoLua/2.0.2`；READY 负载不再附带“调度器不可用”文本。
- 调度器失败日志在 READY 发送后作为独立输出发送；成功候选不输出日志，失败候选包含角色、类名、方法名和具体阶段。
- Hook 准备、候选发现、MinHook 创建/启用和 InternalCall 地址不可用时保留具体错误文本；这些错误只拒绝当前操作或当前候选。

<a name="project-and-documentation"></a>
## 10. 工程与使用文档

- 新增 `src/mono_feature_fault.h` 和 `src/mono_scheduler_candidates.h`，并将它们加入 Visual Studio 工程及筛选器。
- 调整桥接源文件的 Lua API 头文件包含方式，避免同一 DLL 内部链接被错误地按 `dllimport` 处理。
- README 同步说明调度器启动、候选回退、线程确认、泛型限制、错误隔离和诊断输出规则。
- 本版本详细说明保存在本文件；发布平台使用的简短说明保存在 `.release_local/ReleaseOutline_2.0.2.md`，不提交到 Git。
