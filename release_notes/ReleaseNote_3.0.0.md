# MonoLua 3.0.0 Release Notes

## 版本信息

- MonoLua 版本：`3.0.0`
- 协议版本：`MonoLua/3.0.0`
- 配套组件：MonoLua.dll `3.0.0`、HostCore Mono 后端 `3.0.0`、Lune Mono 握手配置 `MonoLua/3.0.0`
- 变更范围：调用与值转换、GCHandle 生命周期、Hook 注册、会话故障隔离和通信日志可靠性重构；协议帧格式保持不变。

<a name="version-and-pairing"></a>
## 1. 版本标识与配套要求

- [src/version.h](../src/version.h) 将 DLL 文件版本、产品版本和协议版本统一为 `3.0.0`，协议字符串为 `MonoLua/3.0.0`。
- [HostCore/src/backend_profile.h](../../HostCore/src/backend_profile.h) 的 Mono profile 和 [Lune README](../../Lune/README.md) 的 HELLO 配置同步为 `3.0.0`；使用该 DLL 时必须配套相同握手字符串的 HostCore/Lune Mono 后端。
- Lune 自身产品版本独立维护；Il2CppLua、UnrealLua 和其他后端的版本配置不随本次更新改变。
- 二进制帧类型、帧头和 1 MiB 单帧负载限制没有改变；HELLO 负载必须精确匹配 `MonoLua/3.0.0`，版本不一致时不会继续进入 READY 或 REPL。

<a name="lua-api-and-userdata"></a>
## 2. Lua 入口与 userdata 模型

- 保留 `lua`、`mono`、`Assembly`、`Class`、`Instance`、`Method` 和 `Field` 的公开入口；使用方式以当前 [README](../README.md) 为准。
- Instance userdata 的句柄类型统一为 `MonoGCHandle`，在支持的运行时优先使用完整宽度的 v2 GCHandle 导出，在旧运行时保留 legacy `uint32_t` 导出回退。句柄所有权在临时调用根、容器访问和 Lua userdata 之间明确转移。
- Instance 同时记录 metadata generation。程序集刷新或运行时关闭后，过期对象、方法和字段引用不会继续作为有效的长期入口使用。
- `mono.wrap(address)` 先检查可读地址和 Mono 对象头的 vtable/domain/class 一致性；校验失败返回 `nil, error`，不会把任意可读元数据地址包装成 Instance。
- Hook、调度回调和输出回调均在进入 Lua 前检查会话状态；已隔离的会话不再复用受损 Lua VM。

<a name="assembly-and-class"></a>
## 3. 程序集与类型元数据

- Mono 导出解析、元数据读取和对象访问拆分为独立模块；required export 缺失会阻止最小运行时初始化，optional export 缺失会保留在 `mono.get_missing_exports()` 和状态诊断中。
- 元数据访问通过不透明 Mono 指针和安全调用边界完成，不复制 Mono 内部结构布局。程序集快照维护 generation，用于在刷新后撤销 Hook、tick、探针和相关队列状态。
- Class、Method、Field 的名称、签名、父类、字段偏移、数组元素类型和类型分类仍通过反射导出查询；单次导出或内存访问失败会转换为当前功能错误，而不是默认值即成功。
- 泛型资格检查收敛到 Hook 元数据准备路径；普通 `Method:call()` 使用 `mono_runtime_invoke`，不因 Hook ABI 检查而扩大或缩小普通托管调用能力。

<a name="method-and-field"></a>
## 4. 方法调用、字段读写和类型转换

- `Class:static_call()`、`Instance:call()` 使用按参数类型评分的重载选择。若同一声明类中存在同分候选，会报告歧义；应通过 `Class:get_method(name, ...)` 精确选出 Method，再调用 `Method:call()`。
- 方法调用集中处理静态/实例目标、参数根、字符串创建、值类型拆箱、返回值固定和托管异常；调用目标和引用参数在整个 `runtime_invoke` 及结果转换期间保持有效。
- 标量封送覆盖 bool、char、窄整数、32/64 位有符号与无符号整数、float、double、enum、字符串、引用对象和装箱值类型。仍不支持原生指针、函数指针、`ref/out`、byref 返回值和 `Nullable<T>`。
- Field 读写与静态字段读写共用值转换路径；引用写入使用 Mono 写屏障，常量和只读约束返回错误，底层读取/写入异常不会被伪装成成功。
- `Method:get_address()` 显式请求 `mono_compile_method` 获取 JIT 地址；打印 Method 或读取签名只显示元数据，不隐式触发 JIT。

<a name="containers-and-object-creation"></a>
## 5. 对象创建、数组与 List<T>

- `Class:new()`、`Class:alloc()` 和 `Class:new_array()` 继续分别表示构造对象、仅分配对象和创建托管数组；值类型可以使用默认零值或装箱后的 Instance 参与转换。
- 一维数组和 `List<T>` 通过 Instance 的 `#instance`、`instance[index]`、`instance[index] = value`、`each()` 和 `dump()` 访问，索引保持 Lua 1 基语义。
- 数组元素按元素类型执行标量、引用和值类型转换；引用写入经过数组写屏障，值类型写入使用复制/装箱路径，容器访问期间固定所属数组对象。
- 容器越界、非整数下标、多维数组、长度超出 32 位范围和缺少所需 Mono 导出时返回错误；普通 Instance 不支持长度运算或数字下标。

<a name="unity-query"></a>
## 6. Unity 对象查询与主线程调度

- `Class:find_unity_objects()` 现在在入口处要求已确认的 Unity 主线程；从控制线程直接调用会返回明确错误，必须使用 `mono.schedule(function() ... end)`。
- 调度任务只在独立探针确认的线程和非嵌套 tick 中执行；`mono.set_tick()` 只替换 tick 入口，不重置已确认的线程身份。
- metadata generation 失效时撤销旧 tick、探针和未交换出去的任务引用，防止旧 Method 或 callback 在新快照上继续执行。
- Unity 查询本身仍只支持继承 `UnityEngine.Object` 的类型，使用已加载程序集中的 `FindObjectsOfType`/`FindObjectsByType` 入口，不扫描完整托管堆。

<a name="hooks"></a>
## 7. Hook 行为、回调规则和限制

- Hook 元数据准备、ABI 校验、入口安装和角色登记拆分为独立阶段；用户 Hook、tick 和主线程探针可以共享同一个 native entry，不会为同一地址重复创建 MinHook。
- Hook 注册采用可回滚的登记顺序。Lua 引用、Hook 表、MinHook trampoline 和角色状态在创建/启用失败时清理半完成状态；替换角色失败时保留旧 Hook 和旧调度状态。
- Hook dispatch 使用快照读取回调、参数和 original 状态；回调失败、返回值不兼容或原生访问异常时恢复原始调用状态，避免把异常继续抛出到游戏线程。
- 元数据失效、单方法移除、`mono.unhook_all()` 和会话关闭都会等待在途 Hook 回调完成；仍在执行的 trampoline 延迟到安全清理点回收。
- 回调格式、参数数量上限、泛型/`ref/out`/byref、结构体 ABI、值类型声明类和 Windows x64 限制保持 README 中的约束；本版本不扩大这些 Hook 支持范围。

<a name="runtime-lifecycle"></a>
## 8. Runtime、metadata generation、对象句柄生命周期和故障隔离

- `MonoResolver` 对 Mono 导出调用、元数据访问、对象访问和字符串/数组操作提供窄 native fault 边界；被捕获的访问异常在当前 Lua 功能返回前转换为带阶段信息的错误。
- `bridge_session` 统一 DLL 工作线程、Mono attach、初始化、前端退出等待和清理顺序；正常关闭按 Hook、Scheduler、Lua、Runtime、Resolver 的顺序释放资源。
- 越过 Lua VM 的 native exception 无法证明 Lua error chain 和 CallInfo 仍可恢复时，会隔离当前 MonoLua 会话并保留故障阶段、异常码和地址；不会清栈后复用受损 VM，也不会在故障路径强行执行不安全的 Mono/Lua 清理。
- Pipe、日志线程、Hook dispatch 和输出回调各自有异常边界。日志写线程故障只关闭日志能力并唤醒等待者，主通道仍可发送命令错误或完成响应。
- 进程终止或故障隔离时可能保留少量原生资源到宿主进程回收；这属于保护游戏线程和避免执行已失效代码的有意取舍。

<a name="protocol-errors-and-io"></a>
## 9. 协议、错误、日志和输出限制

- HELLO 标识更新为 `MonoLua/3.0.0`；READY 的语义和命令响应帧保持不变，调度器诊断不会插入握手帧。
- 管道传输等待重叠 I/O 完成，不再使用过短的写入超时误断开慢速前端；检测到对端关闭、部分帧或无法恢复的帧同步时才关闭连接。
- 异步日志仍有界并按 UTF-8 码点分片；日志队列超限、写线程失败和输出截断不会阻塞主命令响应。过长错误会回退到固定大小的结构化错误负载。
- 单次命令、dump、日志批次和协议帧继续受大小限制；超出限制时返回错误或截断，不会把未消费的载荷当作下一帧头。

<a name="project-and-documentation"></a>
## 10. 工程、依赖和文档调整

- 将桥接会话、Mono metadata/object access、Hook metadata/registry、字段/数组值转换和共享内存安全读写加入 Visual Studio 工程及筛选器。
- 更新 [README](../README.md) 的重载歧义、主线程调用、容器限制、类型映射、Hook 限制和 JIT 地址说明；[ARCHITECTURE](../ARCHITECTURE.md) 记录模块边界、生命周期和发布前验证范围。
- 本文件是 MonoLua 3.0.0 的长期详细记录；发布平台使用的简短内容保存在 `.release_local/ReleaseOutline_3.0.0.md`，不提交到 Git。
