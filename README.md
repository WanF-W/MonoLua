<div align="center">

# 🧩 MonoLua

在 Unity Mono 游戏进程中使用 Lua 检查类型、操作对象、调用方法与 Hook 逻辑

**v1.0.0** · Windows x64 · Lua 5.4.8 · MIT

</div>

## 📑 目录

- [项目定位](#project-positioning)
- [主要能力](#capabilities)
- [快速开始](#quick-start)
- [API 模型](#api-model)
- [API 参考](#api-reference)
  - [`lua`](#api-lua)
  - [`mono`](#api-mono)
  - [`Assembly`](#api-assembly)
  - [`Class`](#api-class)
  - [`Instance`](#api-instance)
  - [`Method`](#api-method)
  - [`Field`](#api-field)
- [Lua 与 Mono 类型映射](#type-mapping)
- [Hook 与线程模型](#hook-threading)
- [通信与版本校验](#protocol-version)
- [MLune 命令行](#mlune-cli)
- [构建](#build)
- [已知限制](#limitations)
- [License](#license)

<a id="project-positioning"></a>

## 🎯 项目定位

MonoLua 是注入 Unity Mono 游戏进程的原生运行时桥接 DLL。它从目标进程中的 Mono
运行时动态解析所需 API，将程序集、类、对象、方法和字段映射为 Lua userdata，
不依赖游戏 SDK 或预生成的 dump 头文件。

配套控制台 [MLune](../MLune) 负责定位进程、注入 DLL、执行 Lua 文件和提供交互式
REPL。v1.0.0 起，MLune 会严格校验 DLL 发送的 HELLO 版本；两个项目必须使用相同版本。

MonoLua 适合使用 Mono/Unity Mono 运行时、并保留所需 `mono_*` 导出函数的 Windows x64
游戏。它不是 IL2CPP 桥接器，也不负责解析未加载的程序集或 metadata 文件。

<a id="capabilities"></a>

## ✨ 主要能力

- 枚举程序集、类、方法和字段
- 在全部程序集或指定程序集内查找类型
- 创建托管对象与一维托管数组
- 读写实例字段和静态字段
- 调用实例方法、静态方法及精确重载
- 遍历和修改数组与 `List<T>`
- 查找存活的 `UnityEngine.Object` 实例
- Hook 原生方法，并在回调中选择调用原实现
- 将 Lua 回调投递到 Unity 主线程
- 将裸对象地址校验后包装为安全的 Instance userdata

<a id="quick-start"></a>

## 🚀 快速开始

将相同版本的 `MonoLua.dll` 与 `mlune.exe` 放在同一目录，启动游戏后执行：

```powershell
mlune.exe -n Game.exe
```

也可以按 PID 注入，或指定 DLL 与启动脚本：

```powershell
mlune.exe -p 1234 -d C:\Tools\MonoLua.dll -l C:\Scripts\startup.lua
```

进入 `mlune >>` 后即可输入 Lua：

```lua
local game = mono.get_assembly("Assembly-CSharp")
local playerClass = game:get_class("Game", "Player")
local players = playerClass:find_unity_objects()

if players and players[1] then
    local player = players[1]
    player:write_field("health", 999)
    print(player:read_field("health"))
    player:call("RefreshStatus")
end
```

示例中的程序集名、命名空间、类名、字段名和方法名必须替换为目标游戏的真实元数据。

<a id="api-model"></a>

## 🧭 API 模型

| 层级 | 用途 |
| --- | --- |
| `lua` | 普通 Lua table 工具 |
| `mono` | 运行时状态、全局查找、Hook 清理和主线程调度 |
| `Assembly` | 程序集范围内的类型查找与枚举 |
| `Class` | 类型信息、创建对象、静态成员和 Unity 对象查找 |
| `Instance` | 实例成员，以及数组和 `List<T>` 容器操作 |
| `Method` | 精确方法信息、调用和 Hook |
| `Field` | 精确字段信息与读写 |

`Assembly`、`Class`、`Method` 和 `Field` 保存当前 Mono 运行时代次的元数据引用。
运行时代次变化后，旧 userdata 会失效；`Instance` 使用强 GCHandle 保持对象存活。

<a id="api-reference"></a>

## 📚 API 参考

### 🧰 `lua`：Lua table 工具

#### `lua.each(table, callback)`

遍历普通 Lua table，回调参数为 `value, key`。

```lua
lua.each({ "a", "b", "c" }, function(value, key)
    print(key, value)
end)

lua.each({ hp = 100, mp = 50 }, function(value, key)
    print(key, value)
end)
```

#### `lua.dump(table)`

输出 table 的第一层键值，不递归展开嵌套对象。

```lua
lua.dump({ name = "Player", stats = { hp = 100 } })
```

#### `lua.hex(value)`

将 Lua integer 或地址格式化为 `0x` 开头的大写十六进制字符串。它只改变显示形式，
不改变地址和偏移 API 的 integer 返回类型。

```lua
print(lua.hex(24))
print(lua.hex(instance:get_address()))
print(lua.hex(field:get_offset()))
```

<a id="api-mono"></a>

### ⚙️ `mono`：运行时入口

| API | 作用 |
| --- | --- |
| `mono.get_status()` | 获取 Mono、程序集与主线程调度状态 |
| `mono.is_initialized()` | 判断 Mono 是否初始化完成 |
| `mono.get_assemblies()` | 枚举全部程序集 |
| `mono.get_assembly(name)` | 按名称查找程序集，忽略大小写并可省略扩展名 |
| `mono.get_class(namespace, name)` | 跨程序集查找类型 |
| `mono.wrap(address)` | 校验并包装裸 Mono 对象地址 |
| `mono.unhook_all()` | 禁用全部用户方法 Hook |
| `mono.schedule(callback)` | 向 Unity 主线程投递任务 |
| `mono.set_tick(method)` | 设置主线程调度 tick 方法 |
| `mono.get_tick()` | 获取当前 tick 方法 |
| `mono.is_tick_ready()` | 判断 tick Hook 是否就绪 |

#### `mono.get_status()` / `mono.is_initialized()`

获取运行时状态，或判断 Mono 是否已经初始化完成。

```lua
print(mono.get_status())
assert(mono.is_initialized(), "Mono runtime is not ready")
```

#### `mono.get_assemblies()` / `mono.get_assembly(name)`

枚举全部程序集，或按名称查找程序集。名称比较忽略大小写，也可以省略 `.dll`。

```lua
lua.each(mono.get_assemblies(), function(assembly)
    print(assembly:get_name())
end)

local game = mono.get_assembly("Assembly-CSharp")
local core = mono.get_assembly("mscorlib.dll")
print(game, core)
```

#### `mono.get_class(namespace, name)`

遍历全部已加载程序集查找类型。存在同名类型时，应使用
`assembly:get_class(namespace, name)` 消除歧义。

```lua
local player = mono.get_class("Game", "Player")
local global = mono.get_class("", "GlobalManager")
print(player, global)
```

#### `mono.wrap(address)`

校验整数地址是否包含有效 Mono 对象，并包装为 Instance。地址当前可读不代表对象长期有效。

```lua
local ok, player = pcall(mono.wrap, 0x000001ABCDEF1230)
if ok then
    print(player:get_class():get_full_name())
else
    print("wrap failed:", player)
end
```

#### `mono.unhook_all()`

移除全部用户方法 Hook；内部主线程 tick Hook 不属于用户 Hook。

```lua
mono.unhook_all()
```

#### `mono.schedule(callback)`

将无参 Lua 函数加入 Unity 主线程队列，不同步返回回调结果。

```lua
mono.schedule(function()
    local time = mono.get_class("UnityEngine", "Time")
    print(time:static_call("get_frameCount"))
end)
```

#### `mono.set_tick(method)` / `mono.get_tick()` / `mono.is_tick_ready()`

设置、查询主线程调度 tick。静态方法和实例方法都可以作为 tick；成功设置后，回调只在
该方法首次运行的线程执行。

```lua
local time = mono.get_class("UnityEngine", "Time")
local tick = time:get_method("get_deltaTime")
mono.set_tick(tick)
print(mono.get_tick())
print(mono.is_tick_ready())
```

<a id="api-assembly"></a>

### 📦 `Assembly`：程序集

#### `assembly:get_name()`

返回程序集镜像名称。

```lua
local assembly = mono.get_assembly("Assembly-CSharp")
print(assembly:get_name())
```

#### `assembly:get_class(namespace, name)`

只在当前程序集内查找类型，不回退到全程序集搜索。

```lua
local game = mono.get_assembly("Assembly-CSharp")
local player = game:get_class("Game", "Player")
local global = game:get_class("", "GlobalManager")
```

#### `assembly:get_classes()`

返回当前程序集声明的全部 Class。

```lua
lua.each(assembly:get_classes(), function(cls)
    print(cls:get_full_name())
end)
```

<a id="api-class"></a>

### 🧬 `Class`：类型

类型信息接口包括 `get_name`、`get_namespace`、`get_full_name`、`get_assembly`、
`get_parent`、`is_value_type`、`is_enum`、`get_instance_size` 和 `get_address`。

其他接口：`get_method` / `get_methods`、`get_field` / `get_fields`、`new`、`alloc`、
`new_array`、`static_call`、`read_static_field`、`write_static_field`、
`find_unity_objects` 和 `dump`。`get_method(name, parameterType...)` 支持按完整参数
类型选择重载，类型别名包括 `bool`、`int`、`float`、`double`、`string`、`object` 等。

#### 类型信息

```lua
local cls = mono.get_class("Game", "Player")
print(cls:get_name())
print(cls:get_namespace())
print(cls:get_full_name())
print(cls:get_assembly())
print(cls:get_parent())
print(cls:is_value_type(), cls:is_enum())
print(cls:get_instance_size())
print(lua.hex(cls:get_address()))
```

#### `class:get_method(name [, parameterType...])` / `class:get_methods()`

不传参数类型时返回第一个同名方法；传入参数类型时精确选择重载。

```lua
local update = cls:get_method("Update")
local byId = cls:get_method("FindItem", "int")
local byName = cls:get_method("FindItem", "System.String")

lua.each(cls:get_methods(), function(method)
    print(method:get_signature())
end)
```

#### `class:get_field(name)` / `class:get_fields()`

查找或枚举当前类声明的字段。

```lua
local health = cls:get_field("health")
print(health:get_signature())

lua.each(cls:get_fields(), function(field)
    print(field:get_signature())
end)
```

#### `class:new(...)` / `class:alloc()`

`new` 分配对象并调用匹配的构造函数；`alloc` 只分配对象，不调用构造函数。

```lua
local empty = cls:new()
local named = cls:new("Wukong")
local raw = cls:alloc()
raw:write_field("health", 100)
```

#### `class:new_array(length)`

创建当前 Class 为元素类型的一维托管数组，Lua 使用 1 基索引。

```lua
local ints = mono.get_class("System", "Int32"):new_array(3)
ints[1], ints[2], ints[3] = 10, 20, 30
print(#ints)

local players = cls:new_array(2)
players[1] = cls:new()
players[2] = nil
```

#### `class:static_call(name, ...)`

按 Lua 实参自动选择并调用静态方法重载。

```lua
local manager = mono.get_class("Game", "PlayerManager")
print(manager:static_call("GetCurrent"))
manager:static_call("SetDifficulty", 2)
```

#### `class:read_static_field(name)` / `class:write_static_field(name, value)`

读写静态字段；实例字段必须使用 Instance 接口。

```lua
local manager = mono.get_class("Game", "PlayerManager")
print(manager:read_static_field("Instance"))
manager:write_static_field("DebugEnabled", true)
```

#### `class:find_unity_objects()` / `class:dump()`

查找当前类型的活跃 Unity 对象，或输出类型概览。

```lua
local enemy = mono.get_class("Game", "EnemyController")
local enemies = enemy:find_unity_objects()
if enemies then
    lua.each(enemies, function(object, index)
        print(index, object:get_address())
    end)
end
enemy:dump()
```

<a id="api-instance"></a>

### 🎮 `Instance`：托管对象与容器

支持 `call`、`read_field`、`write_field`、`get_class`、`get_address`、`dump` 和 `each`。
数组与 `List<T>` 使用 Lua 1 基索引：`#obj` 获取长度，`obj[index]` 读写元素；普通
对象不支持长度运算和数字下标。

#### `instance:call(name, ...)`

按 Lua 实参自动选择实例方法重载。返回值为 `void` 时没有 Lua 返回值。

```lua
local player = cls:new()
player:call("SetLevel", 20)
print(player:call("GetLevel"))
player:call("Teleport", 10.0, 20.0, 30.0)
```

#### `instance:read_field(name)` / `instance:write_field(name, value)`

读写实例字段；静态字段使用 Class 接口。

```lua
print(player:read_field("health"))
player:write_field("health", 999)
player:write_field("target", nil)
```

#### `instance:get_class()` / `instance:get_address()`

获取实际类型和对象地址。

```lua
print(player:get_class():get_full_name())
print(lua.hex(player:get_address()))
```

#### `#instance`、`instance[index]`、`instance[index] = value`、`instance:each(callback)`

数组和 `List<T>` 使用 1 基索引；`each` 回调参数为 `value, index`。

```lua
local inventory = player:read_field("items")
print(#inventory)
print(inventory[1])
inventory[1] = inventory[2]
inventory:each(function(item, index)
    print(index, item)
end)
```

#### `instance:dump([includeParents])`

普通对象默认输出自身字段；传入 `true` 时同时输出父类字段。数组和 List 会直接输出逻辑元素。

```lua
player:dump()
player:dump(true)
inventory:dump()
```

<a id="api-method"></a>

### 🔧 `Method`：精确方法

支持 `get_name`、`get_class`、`get_signature`、`get_address`、`call`、`hook`、
`is_hooked` 和 `unhook`。实例 Hook 回调为 `function(this, original, ...)`；静态 Hook
回调的第一个参数为声明该方法的 Class。

#### 方法信息

```lua
local method = cls:get_method("TakeDamage", "float")
print(method:get_name())
print(method:get_class():get_full_name())
print(method:get_signature())
print(lua.hex(method:get_address()))
```

#### `method:call(instance, ...)`

实例方法第一个参数必须是兼容 Instance；静态方法不传 Instance。

```lua
local getLevel = cls:get_method("GetLevel")
print(getLevel:call(player))

local setLevel = cls:get_method("SetLevel", "int")
setLevel:call(player, 30)

local getCurrent = mono.get_class("Game", "PlayerManager"):get_method("GetCurrent")
print(getCurrent:call())
```

#### `method:hook(callback)`

实例方法的回调签名为 `function(this, original, ...)`。调用 `original()` 时不传参数表示
透传原参数；传入参数则替换原参数。

```lua
local damage = cls:get_method("TakeDamage", "float")
damage:hook(function(this, original, amount)
    print("TakeDamage", amount)
    return original(amount * 0.5)
end)
```

不调用 `original` 时，回调返回值直接作为原生返回值：

```lua
damage:hook(function(this, original, amount)
    return 0
end)
```

静态方法回调的第一个参数是声明 Class：

```lua
local calculate = cls:get_method("CalculateScore", "int")
calculate:hook(function(declaringClass, original, value)
    print(declaringClass:get_full_name(), value)
    return original(value) * 2
end)
```

#### `method:is_hooked()` / `method:unhook()`

```lua
if damage:is_hooked() then
    damage:unhook()
end
```

<a id="api-field"></a>

### 🏷️ `Field`：精确字段

支持 `get_name`、`get_class`、`get_signature`、`get_offset`、`read` 和 `write`。
实例字段操作传入 Instance；静态字段不传 Instance。`const` 字段不可写。

#### 字段信息

```lua
local field = cls:get_field("health")
print(field:get_name())
print(field:get_class():get_full_name())
print(field:get_signature())
print(lua.hex(field:get_offset()))
```

静态字段没有实例偏移，`get_offset()` 返回 `nil`。

#### `field:read(instance)` / `field:write(instance, value)`

实例字段需要传入兼容的 Instance：

```lua
print(field:read(player))
field:write(player, 500)
```

静态字段不传 Instance：

```lua
local instanceField = manager:get_field("Instance")
print(instanceField:read())
instanceField:write(player)
```

<a id="type-mapping"></a>

## 🔄 Lua 与 Mono 类型映射

| Mono 类型 | Lua 表示 |
| --- | --- |
| `bool` | boolean |
| 整数、enum、char | integer |
| `float` / `double` | number |
| `System.String` | string 或 `nil` |
| class / object / array | Instance 或 `nil` |
| struct | 装箱后的 Instance |

Lua integer 是有符号 64 位。引用参数可以传兼容 Instance 或 `nil`。

<a id="hook-threading"></a>

## 🪝 Hook 与线程模型

- Lua VM 由可重入互斥锁串行访问。
- Hook 回调可能来自任意游戏线程。
- 首次 tick 回调会确定 Unity 主线程身份。
- `mono.schedule` 的任务只在已识别的 Unity 主线程 tick 中执行。
- Hook 回调内可以再次调用方法，也可以调用 `original()`。
- 卸载时先禁用 Hook，并保留可能仍被在途调用引用的 trampoline。

不要在高频 Hook 中执行大量打印、文件 IO 或长时间 Lua 计算。只能在主线程访问的
Unity 对象，应通过 `mono.schedule` 操作。

<a id="protocol-version"></a>

## 📡 通信与版本校验

MLune 创建命名管道并注入 DLL，DLL 连接后发送：

```text
MSG_HELLO: MonoLua/1.0.0
```

MLune 会将该字符串与自身协议版本精确比较。版本不同会终止连接，不会进入 READY 或
REPL。通信帧为 `[1 字节类型][4 字节长度 LE][负载]`，单帧负载最大 1 MB。

产品版本、协议版本和 Windows 文件版本都来自两项目各自的 `src/version.h`。发布时
必须保证两个文件内容一致。

<a id="mlune-cli"></a>

## 💻 MLune 命令行

```text
mlune.exe -n <进程名> [-d <DLL路径>] [-l <Lua脚本>]
mlune.exe -p <PID>    [-d <DLL路径>] [-l <Lua脚本>]
```

| 参数 | 说明 |
| --- | --- |
| `-n`, `--name` | 按进程名注入 |
| `-p`, `--pid` | 按 PID 注入 |
| `-d`, `--dll` | 指定 MonoLua.dll，默认查找 MLune 同目录 |
| `-l`, `--lua` | 握手完成后执行 UTF-8 Lua 脚本 |

REPL 中输入表达式会自动作为 `return <表达式>` 执行，语句则原样执行。输入 `exit` 或
`quit` 会结束会话。

<a id="build"></a>

## 🔨 构建

要求：Windows x64、Visual Studio 2022、Windows SDK 10.0、MSVC v145 工具集，以及
MASM x64 构建支持。

打开 `MonoLua.slnx`，选择 `Release | x64` 生成 `MonoLua.dll`。MLune 也应使用
`Release | x64`。正式发布时将相同 v1.0.0 的 DLL 与 EXE 放在一起。

<a id="limitations"></a>

## ⚠️ 已知限制

- 仅支持 Windows x64。
- 依赖目标 Mono 运行时保留所需的 `mono_*` 导出函数。
- `find_unity_objects` 只查找 `UnityEngine.Object`，不遍历完整托管堆。
- 数组、`List<T>` 和部分类型信息接口依赖目标运行时对应导出函数；缺失时会报错或不可用。
- 元数据引用只在当前 Mono 运行时代次内有效；运行时重载后旧 userdata 不可继续使用。
- `get_class` 遇到同名类型时返回第一个结果，应通过 Assembly 消除歧义。
- `get_method(name)` 遇到重载时返回第一个结果，应传参数类型进行精确选择。
- Hook 中的 `ref/out` 参数目前不能通过修改 Lua 参数写回调用方。
- 任意裸地址即使当前可读，也可能在之后因对象销毁或内存复用而失效。

<a id="license"></a>

## 📄 License

MIT License，详见 [LICENSE](LICENSE)。
