# MonoLua 2.0.0 Release Notes

## 版本信息

- MonoLua 版本：`2.0.0`
- 协议版本：`MonoLua/2.0.0`
- 配套组件：MonoLua.dll `2.0.0` 与 Lune `2.0.0`
- 整理依据：MonoLua 工作区相对 Git `HEAD` 的源码、工程文件和文档差异。

<a id="version-and-pairing"></a>
## 1. 版本标识与配套要求

- [src/version.h](../src/version.h) 统一提供 DLL 文件版本、产品版本和协议版本字符串。
- Lune 的 Mono 后端握手标识同步为 `MonoLua/2.0.0`；两端版本不一致时握手失败。
- 工程版本资源继续从 `version.h` 读取，发布包中的 DLL 文件版本与协议版本保持一致。

<a id="lua-api-and-userdata"></a>
## 2. Lua 入口与 userdata 模型

- 全局 `lua` 入口提供 `each`、`dump`、`hex`；`hex` 支持 Lua 整数和 `lightuserdata`。
- 全局 `mono` 入口提供运行状态、程序集/类型查询、地址包装、Hook 和主线程调度接口：
  `get_status`、`get_missing_exports`、`is_initialized`、`get_assemblies`、`get_assembly`、`get_class`、`wrap`、`unhook_all`、`schedule`、`set_tick`、`get_tick`、`is_tick_ready`。
- 程序集、类型、方法、字段和对象分别使用独立 userdata 元表，接口职责拆分为 Assembly、Class、Method、Field、Instance 五类。
- 元数据 userdata 带有 generation 校验；实例 userdata 持有 Mono `GCHandle`，每次访问重新取得对象地址，适配移动式 GC。

<a id="assembly-and-class"></a>
## 3. 程序集与类型元数据

- Assembly 支持名称读取、类型查找和类型枚举。
- Class 支持命名空间、完整名称、程序集、父类、值类型/枚举判断、实例大小和地址查询。
- Class 方法支持方法查找、重载参数类型筛选、方法枚举、字段查找/枚举和继承层级查询。
- 程序集名称查找支持忽略 ASCII 大小写，并允许省略 `.dll` 后缀。
- Class 提供 `static_call`、`read_static_field`、`write_static_field`、`alloc`、`new`、`new_array` 和 `find_unity_objects`。
- 静态字段便捷接口会沿父类查找；实例字段和方法调用也会沿对象类型层级查找。

<a id="method-and-field"></a>
## 4. 方法调用与字段读写

- 方法调用统一使用集中式参数编组和返回值转换，支持基本数值、布尔、字符串、对象引用、数组、枚举以及装箱值类型。
- 实例调用、静态调用和构造函数调用均有明确的参数位置和类型检查；重载选择按 Lua 参数类型和可转换性评分。
- Method 提供签名、JIT 地址、调用、Hook 状态、安装和卸载接口；Field 提供签名、偏移、读取和写入接口。
- Field userdata 同时支持实例字段和静态字段；常量字段不可写，静态字段与实例字段的调用参数形式分别校验。
- `ref`/`out` 参数、`Nullable<T>` 参数或返回值，以及包含未闭合泛型参数的方法调用会明确报错。

<a id="containers-and-object-creation"></a>
## 5. 对象创建、数组与 List<T>

- `Class:alloc()` 分配对象但不调用构造函数；`Class:new(...)` 选择匹配构造函数；值类型可使用无构造参数创建默认值。
- `Class:new_array(length)` 创建一维数组，并检查长度范围。
- 数组和 `System.Collections.Generic.List<T>` 实例支持 `#value`、`value[index]`、`value[index] = newValue` 和 `value:each(callback)`。
- 容器索引统一为 Lua 的 1 起始索引；数组使用 Mono 数组 API，List 使用 `Count`、`get_Item` 和 `set_Item`。
- `dump()` 和字符串表示支持输出对象字段、继承字段、数组和 List 内容，并使用统一的输出长度限制。

<a id="unity-query"></a>
## 6. Unity 对象查询

- `Class:find_unity_objects()` 仅允许查询继承自 `UnityEngine.Object` 的类型。
- 查询优先使用 `UnityEngine.Object.FindObjectsOfType(Type)`，并兼容使用 `FindObjectsByType(Type, FindObjectsSortMode)` 的运行时。
- 查询结果在转换为 Lua Instance 前由强 GC handle 保持，避免遍历或转换过程中对象因 Mono GC 移动而失效。

<a id="hooks"></a>
## 7. Mono JIT Hook

- Hook 使用 Windows x64 原生跳板、MinHook 和独立的 Lua 分发路径，支持实例方法、静态方法、基本参数、对象引用和枚举返回值。
- 回调可使用 `original(...)` 调用原方法；省略参数时复用当前 Hook 捕获的原始参数，也可显式传参。
- 回调返回值可以覆盖原方法返回值；无返回值或回调失败时按 Hook 规则处理原方法回退。
- 同一方法重新 Hook 会替换 Lua 回调；`Method:unhook()`、`mono.unhook_all()` 和元数据失效流程会安全停用回调。
- Hook 安装前拒绝值类型声明类、泛型/膨胀泛型方法、`ref`/`out` 参数、不支持的结构体 ABI、超出 64 个参数的方法，以及共享同一原生入口的不同 Mono 方法。
- 原生异常会使 MonoLua 会话进入故障隔离状态，不继续使用可能已损坏的 Lua VM 或 Mono 调用栈。

<a id="main-thread-scheduler"></a>
## 8. Unity 主线程调度

- `mono.schedule(function)` 将回调放入 Lua registry 队列，由 Unity tick Hook 执行。
- 调度器会自动尝试 `UnityEngine.Time.get_deltaTime`、`UnityEngine.Time.get_frameCount` 和 `UnityEngine.Object.get_name` 作为 tick；也可通过 `mono.set_tick(method)` 指定。
- 首次 tick 固定执行线程身份；不同线程进入时不会消费队列。回调中新加入的任务留到下一次非嵌套 tick。
- `mono.get_tick()` 返回当前 tick 方法签名，`mono.is_tick_ready()` 返回调度器是否已就绪。

<a id="runtime-lifecycle"></a>
## 9. Runtime、元数据与生命周期

- Mono 导出解析按 required/optional 分类；缺少 optional 导出不会直接阻止初始化，可通过 `mono.get_missing_exports()` 查询。
- MonoRuntime 以完整程序集快照提供查询，显式刷新时检测程序集移除并推进 metadata generation。
- MonoSession 统一协调程序集快照变化、Hook 失效和 tick 失效，避免继续使用旧的 Mono 元数据指针。
- 托管临时值、对象参数、方法返回值、数组遍历结果和查询结果均使用作用域 GC handle 管理生命周期。
- 正常退出顺序为停止 Hook、停止调度器、关闭 Lua、关闭 Mono Runtime，最后释放 Resolver 资源。

<a id="protocol-errors-and-io"></a>
## 10. 协议、错误与输出

- 管道帧统一为 `[1 字节类型][4 字节小端长度][负载]`，负载上限为 `1 MiB`。
- `MSG_ERROR` 负载包含错误类别、Lua 行号和 UTF-8 错误文本；Lua、Mono、C#、Lune 等错误来源可被 Lune 区分。
- Lua 语法/运行错误会提取源文件或代码块中的行号；托管异常与桥接错误分别归类。
- 日志通过独立队列和写线程发送，单条日志会按 UTF-8 边界分片；日志队列有条目数和总字节数上限，并报告丢弃数量。
- 命令执行、Hook 回调和调度回调使用独立输出批次；`dump()`、对象打印和方法返回值回显受统一输出上限保护。
- 管道读写使用可取消的重叠 I/O；连接断开、超长帧和写入失败会结束当前通信会话。

<a id="project-and-documentation"></a>
## 11. 工程与使用文档

- Binding、值转换、方法调用、容器、Unity 查询、会话协调和 Dump 输出拆分为独立源码模块，并补充对应内部头文件。
- Visual Studio 工程仅保留 x64 配置，加入新增源码/头文件、MASM Hook 跳板、Lua C++ 异常设置和 MinHook 头文件路径。
- 增加 `.clang-format`、MIT `LICENSE`，并补充 Visual Studio 环境文件的忽略规则。
- [README.md](../README.md) 作为使用手册，仅描述当前安装、启动、Lua API、Hook、调度、构建和限制；本文件承载版本更新详情。
