# `db->layer->feature` 面向数据的协议与内存布局设计规范

本规范旨在解决在 `db -> layer -> feature` 架构下描述复杂数据协议时产生的“概念穿透”、“嵌套树结构”与“复杂多重引用”等问题。通过融合列式内存布局（如 Apache Arrow）与实体组件系统（ECS）的思想，构建一套高性能、无歧义的数据表示层。

---

## 一、 架构核心准则 (The Golden Rules)

为了彻底消除设计中的混乱感，系统必须严格遵守以下三大设计准则：

1. **元数据与数据严格隔离：** 描述“类型结构”（Schema）与存储“真实数据”（Data/Instance）必须使用两套平行的机制，禁止在物理层描述逻辑嵌套。
2. **万物皆扁平池 (Arena Allocation)：** 内存或数据库中不存在任何“嵌套对象”。所有的对象、列表都被彻底打散，分类存储在平铺的一维数组（数据池）中。
3. **引用即 ID (Pointers are IDs)：** 无论是一对一引用、一对多列表、还是多态引用，在物理层绝对不包含目标对象的数据，仅存储目标对象在其所在的池中的整型 ID（Row Index）。

---

## 二、 核心概念重塑

对原有的 `db -> layer -> feature` 进行职责边界的重新明确：

* **`db` (数据库 / 上下文)：** 整个协议或内存实例的最高级命名空间（Context）。它管理所有的 Schema 定义图谱和所有运行时的数据池。
* **`layer` (类型池 / Arena)：** **同一种结构或类型**的数据集合。
  * *逻辑层面：* 它是结构体定义（Struct/Class）。
  * *物理层面：* 它是该类型所有实例的对象池（Object Pool）。
* **`feature` (特征列 / Component)：** `layer` 内部的具体属性。
  * *逻辑层面：* 它是对象的字段（Field）。
  * *物理层面：* 它是一个纯粹的、连续的一维数组（Array/Buffer）。

---

## 三、 元数据系统设计 (Schema / Definition)

元数据子系统负责描述协议的拓扑结构。它本质上是一个“类型注册表”。

### 1. 支持的特征类型 (Feature Kind)

* `PRIMITIVE`: 基础标量（如 `i32`, `f64`, `bool`, `string`）
* `REFERENCE`: 单一引用（存储指向另一个 `layer` 的实体 ID）
* `POLY_REF`: 多态引用（存储目标 `layer_id` + 实体 ID）
* `LIST`: 变长列表（存储在数据池中的 `offset` 与 `length`）

### 2. 协议描述示例 (DSL 表现形式)

假设我们要描述一个“机房监控协议”：一个机房（Room）有多个设备（Device），每个设备记录了多段温度变化（`list[f64]`），且设备引用了一个供应商对象（Vendor）。

```text
// 1. 定义 Vendor 类型
Layer "Vendor" {
    Feature "name": PRIMITIVE(string)
}

// 2. 定义 Device 类型
Layer "Device" {
    Feature "vendor_ref": REFERENCE("Vendor")   // 解决：对象间引用问题
    Feature "temps": LIST(PRIMITIVE("f64"))     // 解决：变长列表嵌套问题
}

// 3. 定义 Room 类型
Layer "Room" {
    Feature "room_id": PRIMITIVE(i32)
    Feature "devices": LIST(REFERENCE("Device")) // 解决：列表嵌套引用问题
}
```

---

## 四、 物理数据系统设计 (Data Storage / Runtime)

在物理运行时，数据流被完全展平为列式存储（Columnar Storage）。内存中没有任何树状结构，只有相互关联的一维数组。

### 1. 基础对象与单一引用的物理布局

**Vendor Layer (实例数据池):**
| row_id (隐式) | name (string 数组) |
| :--- | :--- |
| `0` | "Intel" |
| `1` | "AMD" |

**Device Layer (实例数据池):**
| row_id (隐式) | vendor_ref (i32 数组) | 
| :--- | :--- |
| `0` | `0` (指向 Intel) |
| `1` | `0` (指向 Intel) |
| `2` | `1` (指向 AMD) |

*设计要点：引用类型在物理层仅仅是存储目标 Layer 的 `row_id`。*

### 2. List 嵌套的物理布局 (`list[f64]` 和 `list[ref]`)

List 并不是对象，而是一种基于切片的关系。针对 `Device` 中的 `temps: list[f64]`，底层会自动生成一个专门存储纯数据的隐藏 Layer（例如 `Device_temps_pool`）。

**Device Layer (更新后):**
列表特征列不再存储数据，只存储数据切片的起始位置（`offset`）和长度（`length`）。
数据计算公式：`区间 = [offset, offset + length)`

| row_id | vendor_ref | temps_offset (i32) | temps_length (i32) |
| :--- | :--- | :--- | :--- |
| `0` (设备A) | `0` | `0` | `3` |
| `1` (设备B) | `0` | `3` | `0` (空列表) |
| `2` (设备C) | `1` | `3` | `2` |

**Device_temps_pool Layer (底层纯数据池):**
| row_id | value (f64 数组) |
| :--- | :--- |
| `0` | 35.5 (属设备A) |
| `1` | 36.1 (属设备A) |
| `2` | 37.0 (属设备A) |
| `3` | 40.2 (属设备C) |
| `4` | 41.5 (属设备C) |

*设计要点：无论 list 嵌套多深（如 `list[list[f64]]`），只需增加中间的 offset/length 层。最底层永远是连续的基础类型数组，极大提升 CPU 缓存命中率。*

### 3. 多态引用 (Polymorphic Reference) 的物理布局

若引用目标不固定（如指向 `Vendor` 或 `Room`），`POLY_REF` 类型的 feature 在底层退化为两个独立的特征列：

| row_id | poly_target_layer_id (i32) | poly_target_row_id (i32) |
| :--- | :--- | :--- |
| `0` | `101` (代表 Vendor) | `0` |
| `1` | `102` (代表 Room) | `5` |

---

## 五、 系统运转流程 (Workflow)

当系统接收到外部嵌套网络数据（如 JSON / Protobuf）时，工作流如下：

1. **元数据加载：** `db` 读取 Schema 定义，并在内存中初始化所有的 `layer` 和其内部 `feature` 的动态数组容量。
2. **解析与展平 (Flattening)：**
    * 遇到**对象**：向对应的 `layer` 申请一个新的 `row_id`。
    * 遇到**基础类型**：直接追加 (`push`) 进对应 `feature` 的数组中。
    * 遇到**内部引用**：递归解析子对象，获取其 `row_id`，将该 `row_id` 存入当前对象的引用 `feature` 中。
    * 遇到**列表**：记录当前目标数据池的现有长度作为 `offset`，解析列表内所有元素，统计得出 `length`，将这两个值存入列表特征列。
3. **查询与视图重组：** 业务层根据 Schema 索引，通过 `row_id` 和 `offset` 在各个一维数组中无锁并行读取，将扁平数据按需还原为树状视图给上层应用。

---

## 六、 总结与局限性分析

* **优势：**
  * 完全消除逻辑嵌套与物理存储的概念穿透。
  * 面向数据设计（Data-Oriented Design），内存排布紧凑，序列化/反序列化性能极高。
  * 天然支持任意复杂的图结构（包括树、DAG、循环引用）。
* **局限性：**
  * **删除成本较高：** 纯数组追加性能极佳，但由于引入了全局 ID 引用，删除某一行数据容易产生“悬垂指针”，通常需要配合“标记删除（Tombstone）”或额外的垃圾回收机制（GC）。