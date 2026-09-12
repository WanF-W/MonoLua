<div align="center">

# 🧩 MonoLua

在 Windows x64 Unity Mono 游戏进程中使用 Lua 查找程序集、访问类型和对象、调用方法、读写字段以及 Hook 方法。

</div>

## 📑 目录

- [✅ 使用条件](#usage-conditions)
- [🚀 启动与连接](#quick-start)
- [📚 Lua API](#api-reference)
  - [🧰 `lua`](#api-lua)
  - [⚙️ `mono`](#api-mono)
  - [📦 `Assembly`](#api-assembly)
  - [🧬 `Class`](#api-class)
  - [🎮 `Instance`](#api-instance)
  - [🔧 `Method`](#api-method)
  - [🏷️ `Field`](#api-field)
- [🔄 类型映射](#type-mapping)
- [🪝 线程与 Hook](#hook-threading)
- [💻 Lune 命令行](#lune-cli)
- [🔨 构建](#build)
- [⚠️ 限制](#limitations)

<a name="usage-conditions"></a>
## ✅ 使用条件

- Windows x64。
- 目标程序使用 Unity Mono 运行时。
- 目标程序和 MonoLua.dll 使用兼容的 Mono 导出函数。
- `MonoLua.dll` 与配套的 [Lune](../Lune) 使用同一套构建文件。

MonoLua 读取目标进程中已经加载的 Mono 程序集，不读取未加载的程序集文件，也不适用于 Unity IL2CPP 程序。

<a name="quick-start"></a>
## 🚀 启动与连接

将 `Lune.exe` 和 `MonoLua.dll` 放在同一目录。先启动游戏，再使用 Lune 的 Mono 后端连接目标进程。

按进程名连接：

```bat
Lune.exe -m --name Game.exe
```

按 PID 连接：

```bat
Lune.exe -m --pid 1234
```

指定 DLL 和启动脚本：

```bat
Lune.exe -m --name Game.exe --dll C:\Tools\MonoLua.dll --lua C:\Scripts\startup.lua
```

连接成功后进入 `mlune >>`，可以直接输入 Lua：

```lua
print("hello from MonoLua")
print(mono.get_status())
```

典型的类型查找、对象查找和字段读取流程：

```lua
local game = mono.get_assembly("Assembly-CSharp")
local controller = game:get_class("", "BattleController")

mono.schedule(function()
    local objects, err = controller:find_unity_objects()
    if not objects then
        print("find failed:", err)
        return
    end

    for index, object in ipairs(objects) do
        print(index, object:get_class():get_full_name(), object:get_address())
        print("BattleID:", object:read_field("BattleID"))
    end
end)
```

`find_unity_objects()` 需要在 Unity 主线程调用，因此放在 `mono.schedule()` 中。字段名、方法名、命名空间和类型名必须替换为目标游戏中的实际元数据。

<a name="api-reference"></a>
## 📚 Lua API

| 模块 | 说明 |
| --- | --- |
| [`lua`](#api-lua) | Lua 辅助函数 |
| [`mono`](#api-mono) | Mono 运行时入口、查找与调度 |
| [`Assembly`](#api-assembly) | 程序集查询 |
| [`Class`](#api-class) | 类型查询、对象创建与静态成员操作 |
| [`Instance`](#api-instance) | 实例、数组与 `List<T>` 操作 |
| [`Method`](#api-method) | 方法信息、调用与 Hook |
| [`Field`](#api-field) | 字段信息、读取与写入 |

<a name="api-lua"></a>
### 🧰 `lua`

<a name="api-lua-each"></a>
#### 🔁 `lua.each(table, callback)`

遍历 Lua table。回调参数为 `value, key`。

```lua
lua.each({ "a", "b", "c" }, function(value, key)
    print(key, value)
end)

lua.each({ hp = 100, mp = 50 }, function(value, key)
    print(key, value)
end)
```

<a name="api-lua-dump"></a>
#### 🧾 `lua.dump(table)`

输出 Lua table 的第一层键值，不递归展开嵌套 table。

```lua
lua.dump({ name = "Player", stats = { hp = 100 } })
```

<a name="api-lua-hex"></a>
#### 🔢 `lua.hex(value)`

将整数或地址格式化为十六进制字符串。它不改变原值。

```lua
print(lua.hex(24))
print(lua.hex(object:get_address()))
print(lua.hex(field:get_offset()))
```

<a name="api-mono"></a>
### ⚙️ `mono`

| API | 返回值或作用 |
| --- | --- |
| `mono.get_status()` | 返回运行时、程序集、镜像、调度器、模块和日志状态文本 |
| `mono.get_missing_exports()` | 返回缺失的可选 Mono 导出名称 table |
| `mono.is_initialized()` | 返回 Mono 是否初始化完成 |
| `mono.get_assemblies()` | 返回全部程序集 table |
| `mono.get_assembly(name)` | 返回指定 Assembly，找不到时返回 `nil` |
| `mono.get_class(namespace, name)` | 跨程序集查找 Class，找不到时返回 `nil` |
| `mono.wrap(address)` | 校验地址并返回 Instance，失败时返回 `nil, error` |
| `mono.unhook_all()` | 移除用户安装的全部 Hook |
| `mono.schedule(callback)` | 将无参 Lua 回调加入主线程任务队列 |
| `mono.set_tick(method)` | 设置任务调度使用的 tick 方法 |
| `mono.get_tick()` | 返回 tick 方法签名，未设置时返回 `nil` |
| `mono.is_tick_ready()` | 返回 tick Hook 是否已安装，不表示主线程探针已确认线程 |

<a name="mono-runtime-status"></a>
#### 📊 运行时状态

```lua
assert(mono.is_initialized(), "Mono runtime is not ready")
print(mono.get_status())

for _, name in ipairs(mono.get_missing_exports()) do
    print("missing export:", name)
end
```

状态文本使用与 `il2cpp.get_status()` 一致的 `Initialized`、`Exports`、`Assemblies`、`Images`、`Main thread` 和 `Rejected log batches` 字段；Mono 额外报告 Root/Worker domain、metadata generation 与 Mono 模块地址。`Main thread` 表示调度 Hook 已安装，实际执行线程仍须由主线程探针确认。Mono 当前只统计提交阶段被拒绝的日志批次，未将传输失败或关闭时清空的分片混入该计数。

<a name="mono-assembly-search"></a>
#### 🔎 程序集查找

程序集名称不区分大小写，可以带或不带 `.dll` 后缀。

```lua
local assemblies = mono.get_assemblies()
for _, assembly in ipairs(assemblies) do
    print(assembly:get_name())
end

local game = mono.get_assembly("Assembly-CSharp")
local core = mono.get_assembly("mscorlib.dll")
local player = mono.get_class("Game", "Player")
```

`mono.get_class()` 在全部已加载程序集内查找。存在同名类型时，使用 `assembly:get_class()` 指定程序集。

<a name="mono-address-wrap"></a>
#### 📍 地址包装

`mono.wrap()` 接受整数或 `lightuserdata` 地址，并返回经过运行时校验的 Instance。

```lua
local object, err = mono.wrap(0x000001ABCDEF1230)
if object then
    print(object:get_class():get_full_name())
else
    print("wrap failed:", err)
end
```

地址可能因对象销毁、内存复用或运行时状态变化而失效，不要长期保存未经验证的裸地址。

<a name="mono-main-thread"></a>
#### 🧵 主线程调度

`mono.schedule(callback)` 不同步返回回调结果。回调必须是无参函数；成功调用表示任务已经入队。

```lua
mono.schedule(function()
    local time = mono.get_class("UnityEngine", "Time")
    print(time:static_call("get_frameCount"))
end)
```

自动 tick 会尝试使用 `UnityEngine.Time` 的帧入口。也可以手动指定：

```lua
local time = mono.get_class("UnityEngine", "Time")
local tick = time:get_method("get_deltaTime")

local ok, err = mono.set_tick(tick)
if not ok then
    print("set tick failed:", err)
end

print(mono.get_tick())
print(mono.is_tick_ready())
```

任务队列最多保存 1024 项；队列满时 `mono.schedule()` 抛出错误。没有可用 tick 时，任务会保留到 tick 设置完成后执行。

线程身份由独立的一次性探针确认，优先使用 `UnitySynchronizationContext.ExecuteTasks`，安装失败时尝试 `Time.get_deltaTime`。控制台 Lua、MonoLua 的 `runtime_invoke` 和嵌套 Hook 不参与确认；确认后撤销探针身份，只保留同一入口上的 tick 或用户 Hook。切换 tick 不会重置线程身份，探针或 tick 安装失败不会丢弃已入队任务，可再次调用 `mono.set_tick()` 重试。

`get_deltaTime` 后备探针依赖游戏在主线程自然调用该入口；它无法排除游戏自身工作线程的直接调用。优先探针安装成功但从未被游戏调用时，队列会继续等待，`is_tick_ready()` 仍可能为 `true`。

InternalCall Hook 使用 `mono_lookup_internal_call` 获取原生地址，目前支持 `UnityEngine.Time` 中静态、无参数、数值返回的 getter（包括 `get_deltaTime()`、`get_frameCount()`、`get_timeScale()`、`get_unscaledDeltaTime()` 和 `get_realtimeSinceStartup()`）。带对象、结构体、参数或隐藏 ABI 的 InternalCall 会返回明确错误；普通 Mono 方法仍使用 JIT 地址。不同方法共享同一 native 地址时拒绝重复注册，MinHook 安装失败会保留具体状态。

<a name="api-assembly"></a>
### 📦 `Assembly`

Assembly 表示一个已加载的 Mono 程序集。

| API | 作用 |
| --- | --- |
| `assembly:get_name()` | 返回程序集名称 |
| `assembly:get_class(namespace, name)` | 在该程序集内查找 Class |
| `assembly:get_classes()` | 返回该程序集声明的全部 Class |

```lua
local game = mono.get_assembly("Assembly-CSharp")
print(game:get_name())

local player = game:get_class("Game", "Player")
local global = game:get_class("", "GlobalManager")

for _, class in ipairs(game:get_classes()) do
    print(class:get_full_name())
end
```

<a name="api-class"></a>
### 🧬 `Class`

Class 表示一个 Mono 类型。

| API 区域 | 说明 |
| --- | --- |
| [类型信息](#class-information) | 查看类型名称、继承关系、大小和地址 |
| [方法查找](#class-methods) | 查找方法和重载 |
| [字段查找](#class-fields) | 查找字段 |
| [创建对象](#class-new) | 分配并构造对象 |
| [创建数组](#class-array) | 创建托管数组 |
| [静态方法](#class-static-call) | 调用静态方法 |
| [静态字段](#class-static-field) | 读取和写入静态字段 |
| [Unity 对象查找](#class-find-unity-objects) | 查询场景中的 Unity 对象 |

<a name="class-information"></a>
#### 📋 类型信息

| API | 返回值 |
| --- | --- |
| `class:get_name()` | 类型名 |
| `class:get_namespace()` | 命名空间 |
| `class:get_full_name()` | 完整类型名 |
| `class:get_assembly()` | 声明该类型的 Assembly |
| `class:get_parent()` | 父类 Class，没有父类时为 `nil` |
| `class:is_value_type()` | 是否为值类型 |
| `class:is_enum()` | 是否为枚举 |
| `class:get_instance_size()` | 实例大小，无法取得时为 `nil` |
| `class:get_address()` | `MonoClass` 地址 |
| `class:dump()` | 输出类型信息 |

```lua
local class = mono.get_class("Game", "Player")
print(class:get_name())
print(class:get_namespace())
print(class:get_full_name())
print(class:get_assembly():get_name())
print(class:get_parent())
print(class:is_value_type(), class:is_enum())
print(class:get_instance_size())
print(lua.hex(class:get_address()))
class:dump()
```

<a name="class-methods"></a>
#### 🔍 方法查找

```lua
local update = class:get_method("Update")
local byId = class:get_method("FindItem", "int")
local byName = class:get_method("FindItem", "System.String")

for _, method in ipairs(class:get_methods()) do
    print(method:get_signature())
end
```

`get_method(name)` 返回第一个同名方法。存在重载时传入参数类型进行精确查找：

```lua
local threeStrings = class:get_method("Find", "string", "string", "string")
local stringArray = class:get_method("Find", "string[]")
```

支持完整 Mono 类型名、末级类型名、基础别名和一维数组后缀。常用别名包括 `bool`、`int`、`uint`、`long`、`float`、`double`、`string` 和 `object`。

`get_method()` 会沿父类查找普通方法；构造函数 `.ctor` 和类型初始化方法 `.cctor` 只在声明类查找。`get_methods()` 只枚举该 Class 声明的方法。

<a name="class-fields"></a>
#### 🏷️ 字段查找

```lua
local health = class:get_field("health")
print(health:get_signature())

for _, field in ipairs(class:get_fields()) do
    print(field:get_signature())
end
```

`get_field()` 和 `get_fields()` 只处理该 Class 声明的字段。Instance 字段读写以及 Class 的静态字段便捷接口会沿父类查找。

<a name="class-new"></a>
#### 🆕 创建对象

```lua
local object = class:new(arg1, arg2)
local raw = class:alloc()
```

`new()` 会查找匹配的构造函数并执行；`alloc()` 只分配对象，不执行构造函数。值类型无参数且没有显式构造函数时，`new()` 返回默认值对象。

<a name="class-array"></a>
#### 🧱 创建数组

```lua
local intClass = mono.get_class("System", "Int32")
local values = intClass:new_array(3)
values[1], values[2], values[3] = 10, 20, 30
print(#values)

local objects = class:new_array(2)
objects[1] = class:new()
objects[2] = nil
```

数组使用 Lua 1 基索引，长度必须是非负整数。

<a name="class-static-call"></a>
#### ⚡ 静态方法

```lua
local manager = mono.get_class("Game", "PlayerManager")
print(manager:static_call("GetCurrent"))
manager:static_call("SetDifficulty", 2)
```

`static_call()` 会根据 Lua 参数选择静态方法重载，并沿父类查找。

<a name="class-static-field"></a>
#### 🗂️ 静态字段

```lua
local manager = mono.get_class("Game", "PlayerManager")
print(manager:read_static_field("Instance"))
manager:write_static_field("DebugEnabled", true)
```

`read_static_field()` 和 `write_static_field()` 会沿父类查找静态字段。常量字段不可写。

<a name="class-find-unity-objects"></a>
#### 🎮 Unity 对象查找

```lua
local enemy = mono.get_class("Game", "EnemyController")

mono.schedule(function()
    local enemies, err = enemy:find_unity_objects()
    if not enemies then
        print("find failed:", err)
        return
    end

    for index, object in ipairs(enemies) do
        print(index, object:get_address())
    end
end)
```

查询类型必须继承 `UnityEngine.Object`。返回值是 Instance table；没有匹配对象时返回空 table，查询失败时返回 `nil, error`。

<a name="api-instance"></a>
### 🎮 `Instance`

Instance 表示一个托管对象，也用于表示数组、`List<T>` 和装箱后的值类型。

| API 区域 | 说明 |
| --- | --- |
| [实例信息](#instance-information) | 查看实例类型、地址和字段 |
| [实例方法](#instance-methods) | 调用实例方法 |
| [实例字段](#instance-fields) | 读取和写入实例字段 |
| [数组与 List](#instance-containers) | 访问数组和 `List<T>` |

<a name="instance-information"></a>
#### 📋 实例信息

```lua
print(object:get_class():get_full_name())
print(lua.hex(object:get_address()))
print(object)
```

| API | 作用 |
| --- | --- |
| `instance:get_class()` | 返回实际类型 Class |
| `instance:get_address()` | 返回托管对象地址 |
| `instance:dump()` | 输出实例字段 |
| `instance:dump(true)` | 连同父类实例字段一起输出 |

<a name="instance-methods"></a>
#### 📞 实例方法

```lua
local player = class:new()
player:call("SetLevel", 20)
print(player:call("GetLevel"))

local getLevel = class:get_method("GetLevel")
print(getLevel:call(player))
```

`instance:call(name, ...)` 根据 Lua 参数选择实例方法重载。`void` 方法没有 Lua 返回值。

<a name="instance-fields"></a>
#### 📝 实例字段

```lua
print(player:read_field("health"))
player:write_field("health", 999)
player:write_field("target", nil)
```

静态字段必须使用 `Field:read/write` 或 `Class:read_static_field/write_static_field`。

<a name="instance-containers"></a>
#### 📚 数组与 List

数组和 `List<T>` 使用 Lua 1 基索引：

```lua
print(#items)
print(items[1])
items[1] = items[2]

items:each(function(value, index)
    print(index, value)
end)

items:dump()
```

普通对象不支持长度运算或数字下标。容器越界、非整数下标和多维数组会抛出错误。

<a name="api-method"></a>
### 🔧 `Method`

| API | 作用 |
| --- | --- |
| `method:get_name()` | 方法名 |
| `method:get_class()` | 声明方法的 Class |
| `method:get_signature()` | 方法签名 |
| `method:get_address()` | 已编译的原生入口地址，无法取得时为 `nil` |
| `method:call(instance, ...)` | 精确调用方法 |
| `method:hook(callback)` | 安装 Lua Hook |
| `method:is_hooked()` | 查询 Hook 状态 |
| `method:unhook()` | 移除该方法的 Hook |

实例方法的 `call()` 需要先传 Instance；静态方法不传 Instance：

```lua
local getLevel = class:get_method("GetLevel")
local getCurrent = class:get_method("GetCurrent")

print(getLevel:call(player))
print(getCurrent:call())
```

<a name="api-field"></a>
### 🏷️ `Field`

| API | 作用 |
| --- | --- |
| `field:get_name()` | 字段名 |
| `field:get_class()` | 声明字段的 Class |
| `field:get_signature()` | 字段类型签名 |
| `field:get_offset()` | 实例字段偏移；静态字段返回 `nil` |
| `field:read(instance)` | 读取字段 |
| `field:write(instance, value)` | 写入字段 |

实例字段传入 Instance：

```lua
local field = class:get_field("health")
print(field:get_name())
print(field:get_signature())
print(lua.hex(field:get_offset()))
print(field:read(player))
field:write(player, 500)
```

静态字段不传 Instance：

```lua
local instanceField = class:get_field("Instance")
print(instanceField:read())
instanceField:write(player)
```

常量字段不可写。值类型字段可以写入装箱后的值类型 Instance；传入 `nil` 表示值类型默认零值。

<a name="type-mapping"></a>
## 🔄 类型映射

| Mono 类型 | Lua 类型 |
| --- | --- |
| `bool` | boolean |
| 整数、enum、char | integer |
| `float`、`double` | number |
| `System.String` | string 或 `nil` |
| class、object、array | Instance 或 `nil` |
| struct | 装箱后的 Instance |

指针地址 API 返回 Lua integer；`mono.wrap()` 也接受 `lightuserdata`。`UInt64` 和 `UIntPtr` 保留 64 位位模式，窄整数进行范围检查。

引用类型参数和字段可以使用兼容的 Instance 或 `nil`。struct 参数、字段和数组元素可以使用装箱后的值类型 Instance 或 `nil` 默认值。

不支持原生指针类型、函数指针、`ref/out`、byref 返回值和 `Nullable<T>`。

<a name="hook-threading"></a>
## 🪝 线程与 Hook

### 🧵 Unity 主线程

MonoLua 命令可以从控制线程执行，但 Unity API 和 Unity 对象应在 Unity 主线程使用：

```lua
mono.schedule(function()
    local gameObject = mono.get_class("UnityEngine", "GameObject")
    local objects, err = gameObject:find_unity_objects()
    if objects then
        print(#objects)
    else
        print(err)
    end
end)
```

`mono.schedule()` 的任务只在探针确认的线程、非嵌套 tick 上执行。`mono.set_tick()` 选择调度入口，不改变已确认的线程身份；元数据刷新或会话关闭会清理线程身份和队列。

### 🪝 Hook 回调

实例方法回调格式：

```lua
local damage = class:get_method("TakeDamage", "float")
damage:hook(function(this, original, amount)
    print("TakeDamage", amount)
    return original(amount * 0.5)
end)
```

静态方法回调的第一个参数是声明该方法的 Class：

```lua
local calculate = class:get_method("CalculateScore", "int")
calculate:hook(function(declaringClass, original, value)
    print(declaringClass:get_full_name(), value)
    return original(value) * 2
end)
```

`original()` 不传参数时透传本次 Hook 的原始参数；传入参数时使用新参数调用原方法。回调也可以不调用 `original()`，直接返回替代结果。

```lua
damage:hook(function(this, original, amount)
    return 0
end)

if damage:is_hooked() then
    damage:unhook()
end

mono.unhook_all()
```

Hook 回调可能在任意游戏线程执行。高频 Hook 中不要进行大量打印、文件 IO 或长时间 Lua 计算。Hook 只支持 Windows x64；泛型方法、实例化泛型方法、包含 `ref/out` 或 byref 的方法、结构体 ABI 方法和值类型声明类不能 Hook，方法参数最多 64 个。

<a name="lune-cli"></a>
## 💻 Lune 命令行

```text
Lune.exe -m --name <进程名> [--dll <DLL路径>] [--lua <脚本路径>]
Lune.exe -m --pid <PID>    [--dll <DLL路径>] [--lua <脚本路径>]
```

| 参数 | 说明 |
| --- | --- |
| `-m` | 使用 MonoLua 后端 |
| `-n`、`--name` | 按进程名查找目标进程 |
| `-p`、`--pid` | 按 PID 查找目标进程 |
| `-d`、`--dll` | 指定 MonoLua.dll 路径；默认查找 Lune.exe 同目录 |
| `-l`、`--lua` | 连接并初始化后执行启动 Lua 脚本 |
| `-h`、`--help` | 显示帮助 |

表达式在 REPL 中会自动回显结果；语句、函数定义和控制流按原样执行。输入 `exit` 或 `quit` 结束会话。

启动脚本示例：

```bat
Lune.exe -m --name Game.exe --lua C:\Scripts\startup.lua
```

`--lua` 的相对路径以启动 Lune 的工作目录为基准。脚本由目标游戏进程读取，因此目标进程必须能够访问该路径。

在 REPL 中使用 `dofile()` 时，相对路径以游戏进程工作目录为基准；需要固定路径时使用绝对路径：

```lua
dofile([[C:\Scripts\test.lua]])
```

<a name="build"></a>
## 🔨 构建

构建环境：

- Windows x64。
- Visual Studio，包含 MSVC v145 工具集。
- Windows SDK 10.0。
- MASM x64 构建支持。

在 Visual Studio 中打开 `MonoLua.slnx`，选择 `Release | x64` 生成 `MonoLua.dll`。

<a name="limitations"></a>
## ⚠️ 限制

- 仅支持 Windows x64 Unity Mono 进程。
- 只能访问已经加载到目标进程的程序集和类型。
- `find_unity_objects()` 只查询继承自 `UnityEngine.Object` 的类型，不扫描完整托管堆。
- `get_class()` 遇到同名类型时应使用指定 Assembly 的 `get_class()` 消除歧义。
- `get_method(name)` 遇到重载时应传入参数类型。
- 数组和 `List<T>` 使用 Lua 1 基索引；只支持一维数组。
- 数组长度和容器索引使用 32 位整数范围。
- 普通调用不支持 `ref/out`、byref 返回值和 `Nullable<T>`。
- Hook 不支持泛型方法、实例化泛型方法、结构体 ABI 方法和值类型声明类。
- 元数据或托管对象失效后，相关 Lua userdata 不能继续使用。
- `mono.wrap()` 得到的 Instance 只在对象仍然有效时可用。
- dump 输出、启动脚本和日志传输均有大小限制；超出限制时输出会被截断或返回错误。
