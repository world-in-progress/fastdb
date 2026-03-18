# fastdb4ts 实施方案

## 当前进度

- P0：WASM 构建骨架已完成
- P1：Embind 核心绑定已完成，并通过真实 WASM smoke test
- P2：TypeScript 类型系统 + Feature 已完成，并通过纯 TS smoke test
- P3：ORM + Table + 列访问 已完成，并通过 ORM roundtrip smoke test
- P4：FastSerializer 已完成，并通过 TS smoke + TS/Python 双向互操作测试
- P5：Packaging 已完成，并补齐 `./tests/ts` 纯 TS 测试与打包 dry-run 校验
- 当前阶段：P0-P5 全部完成

## 目标

在现有 `fastdb` C++ 内核与 `fastdb4py` Python 绑定基础上，设计并实现一套面向前端浏览器的 TypeScript 绑定：`fastdb4ts`。

该方案的目标不是简单“把 C++ 暴露给 JS”，而是：

- 让浏览器端可以直接使用 fastdb 的二进制存储能力
- 复刻 fastdb4py 的核心开发体验：类型系统、Feature、ORM、Table、列访问、FastSerializer
- 用二进制协议替代 JSON / JSON-RPC，支持模型参数传输、仿真过程数据交换、图形可视化资源调度等高吞吐场景
- 保持与 Python 端在数据模型与序列化协议上的兼容性，形成前后端统一的数据交换层

当前已经确认的边界：

- **运行环境**：浏览器端优先
- **共享内存 IPC**：暂不做
- **文件持久化**：浏览器端不做文件 save/load，改为 `ArrayBuffer` / `Uint8Array` 的导入导出
- **优先级**：序列化器优先级很高，但基础仍需要先搭建 WASM + 内核绑定 + ORM 主干

---

## 一、现状分析

### 1. 现有 fastdb 分层

当前代码库已经具备比较清晰的多层结构：

1. **C++ Core**：`fastcarto/fastdb/`
   - 真正的数据存储、表管理、字段布局、序列化逻辑都在这里
   - 对外核心对象包括：
     - `FastVectorDbBuild`
     - `FastVectorDbLayerBuild`
     - `FastVectorDb`
     - `FastVectorDbLayer`
     - `FastVectorDbFeature`
     - `MemoryStream`

2. **Python Binding**：`fastcarto/fastdb/swig/fastdb4py.i`
   - 已经定义了面向 Python 的 API 暴露面
   - 包括重命名规则、字段访问接口、批量读写接口、NumPy 零拷贝桥接

3. **Python 高层封装**：`python/fastdb4py/`
   - `type.py`：字段类型系统
   - `feature/feature.py`：Feature 基类、字段 dispatch
   - `orm/__init__.py`：ORM 生命周期
   - `orm/table.py`：Table 与 column accessor
   - `serializer.py`：FastSerializer 对象图序列化

这意味着：**fastdb4ts 不应该重写内核，而应该最大化复用 C++ Core，同时在 TypeScript 层复刻 Python 的高层语义。**

### 2. SWIG 不能直接用于浏览器 WASM

已经明确结论：

- SWIG 的 JavaScript 目标主要面向 Node/V8 原生扩展
- 不能直接产出浏览器可运行的 WASM 绑定
- 现有 `fastdb4py.i` 仍然很有价值，因为它相当于一份“绑定接口规范”

因此，**fastdb4ts 的 C++ 绑定层应采用 Emscripten + Embind**。

### 3. 现有 CMake 中的 Emscripten 状态

现有 `fastcarto/fastdb/CMakeLists.txt` 中：

- 已有 `is_emscripten` 判断
- 但末尾存在一个历史遗留的 `emcc` post-build 片段
- 该片段依赖的 `fastdb4em.cxx` / `fastdb4em_i.js` 并不存在
- 同时 `BUILD_TYPE` 虽然设置了 `STATIC` / `SHARED`，但 `add_library` 实际写死为 `SHARED`

结论：

- 当前仓库**并没有可用的 WASM 绑定实现**
- 但已有一些为 Emscripten 预留过的痕迹
- 实施时应当**清理废弃路径**，建立新的独立 `ts/embind` 构建入口

---

## 二、总体设计原则

### 原则 1：C++ 内核与 Embind 必须隔离

这是本方案的最高优先级约束之一。

要求：

- 不把 Embind 代码混入 `fastcarto/fastdb/src/`
- 不让普通 Python 构建依赖 Emscripten
- 不让 `fastcarto` 的常规构建流程感知 `fastdb4ts`
- 只有在明确执行 WASM 构建时才编译 Embind 模块

因此采取以下结构：

- `fastcarto/`：保留核心库
- `ts/embind/`：放置所有 Embind 绑定代码与独立 CMake 入口
- `ts/fastdb4ts/`：放置 TypeScript 高层包

### 原则 2：以 Python API 语义为参照，不机械复制实现

fastdb4py 中有很多 API 语义值得复刻，但 TS 运行环境与 Python 不同，因此应该：

- **复刻能力与使用体验**
- **不机械照搬技术实现**

例如：

- Python 的 `__array_interface__` → TS 中改为 `TypedArray + StridedColumn`
- Python 的 `__getattr__`/`__setattr__` → TS 中改为 `Proxy`
- Python 的 `ORM.load(file)` → TS 中改为 `ORM.fromBuffer(buffer)`

### 原则 3：二进制协议兼容优先于 API 完全一致

最重要的不是名字完全一样，而是：

- Python 能序列化的数据，TS 能反序列化
- TS 能构建的数据，Python 能读取
- 两端共享统一 schema / field layout / ref 编码

其中最关键的是 `FastSerializer` 协议兼容。

### 原则 4：每个阶段性成功后立即提交

为了防止后续阶段覆盖中间状态、影响分析和回滚，实施过程中增加一条工程约束：

- 每完成一个阶段（P0 / P1 / P2 ...）并通过该阶段的最小验收后，**必须立即提交一次 git commit**
- commit 只包含该阶段相关文件，不混入无关改动
- 若存在分析文档、临时构建目录、产物文件，应通过 `.gitignore` 或选择性暂存隔离

后续开发默认遵守这条准则。

---

## 三、目标能力映射

下面给出 `fastdb4py` 到 `fastdb4ts` 的核心能力映射。

| Python 能力 | TS 对应方案 | 是否第一阶段实现 |
|---|---|---|
| `Feature` 子类 + 类型注解 | `Feature` 子类 + `defineSchema()` | 是 |
| `ORM.create()` | `ORM.create()` | 是 |
| `ORM.truncate()` | `ORM.truncate()` | 是 |
| `ORM.load(..., from_file=False)` | `ORM.fromBuffer()` | 是 |
| `db.save()` | `db.toBuffer()` | 是 |
| `table.column.x` | `table.column.x`（返回 `StridedColumn`） | 是 |
| `table[i]` | `table.get(i)` 或代理索引 | 是 |
| `iter_reuse()` | `iterReuse()` | 是 |
| `FastSerializer.dumps/loads` | `FastSerializer.dumps/loads` | 是 |
| shared memory | 暂不实现 | 否 |
| 文件系统持久化 | 暂不实现 | 否 |

---

## 四、目录与模块设计

建议的仓库内目录结构如下：

```text
ts/
├── embind/
│   ├── CMakeLists.txt
│   ├── fastdb4ts.cpp
│   └── post.js
├── fastdb4ts/
│   ├── package.json
│   ├── tsconfig.json
│   ├── README.md
│   └── src/
│       ├── index.ts
│       ├── wasm-loader.ts
│       ├── types.ts
│       ├── schema.ts
│       ├── feature.ts
│       ├── orm.ts
│       ├── table.ts
│       ├── column.ts
│       ├── serializer.ts
│       ├── errors.ts
│       └── wasm/
│           ├── fastdb4ts.js
│           └── fastdb4ts.wasm
├── tests/
│   ├── test-types.ts
│   ├── test-feature.ts
│   ├── test-orm.ts
│   ├── test-column.ts
│   ├── test-serializer.ts
│   └── fixtures/
└── build-wasm.sh
```

### 各模块职责

#### `ts/embind/`

负责 C++ 到 WASM 的桥接。

- `fastdb4ts.cpp`
  - 定义 `EMSCRIPTEN_BINDINGS`
  - 只做“绑定”，不写业务逻辑
  - 不改内核数据结构

- `CMakeLists.txt`
  - 独立构建入口
  - 强制要求 Emscripten toolchain
  - 通过 `add_subdirectory(../../fastcarto)` 复用核心库

#### `ts/fastdb4ts/src/`

负责 TS 高层 API。

- `types.ts`
  - 字段类型常量与映射
- `schema.ts`
  - schema 定义、字段索引缓存
- `feature.ts`
  - Feature 基类 + Proxy 行为
- `orm.ts`
  - ORM 生命周期
- `table.ts`
  - 表访问、行迭代、映射对象
- `column.ts`
  - 零拷贝/低拷贝列视图
- `serializer.ts`
  - 对象图序列化协议
- `wasm-loader.ts`
  - Emscripten 模块初始化、实例缓存、错误处理

---

## 五、技术路线细化

## 5.1 C++ / WASM 绑定层

### 目标

建立一个最小但完整的 Embind 层，使 TS 可以访问 C++ 核心的以下能力：

- 构建数据库
- 构建 layer / feature
- 从内存 buffer 加载数据库
- 访问表、行、字段
- 获取底层 buffer 指针与长度
- 获取 feature 地址、field offset、feature stride
- 读取 batch scalar fields

### 绑定对象范围

第一批建议绑定：

- `MemoryStream`
- `chunk_data_t`
- `FastVectorDbBuild`
- `FastVectorDbLayerBuild`
- `FastVectorDb`
- `FastVectorDbLayer`
- `FastVectorDbFeature`
- `FastVectorDbFeatureRef`

### 绑定策略

#### 1. 只暴露稳定 API，不暴露内部实现

不暴露：

- `Impl*`
- 几何回调类 `GeometryReturn`
- 不必要的 tile / geometry 复杂接口

优先暴露：

- 当前 Python binding 已经在用的接口
- ORM / serializer 必需接口

#### 2. 增加少量“WASM 友好 wrapper”

因为 Embind 不像 SWIG 一样直接帮你处理很多语言映射，所以建议添加少量 wrapper：

- `get_field_defn_json()` 或结构化返回
- `load_from_heap(offset, size)`
- `buffer_ptr()` / `buffer_size()`
- `feature_address()`
- `read_fields_to_heap()`

这些 wrapper 应该写在 `ts/embind/fastdb4ts.cpp` 中，而不是污染内核头文件。

#### 3. 指针转换统一由 TS 层封装

原则：

- C++ 只返回 pointer / size / offset
- TS 层统一负责把它转换成 `TypedArray`

例如：

```ts
const byteOffset = feature.getAddress() + layer.getFieldOffset(fieldIndex);
const stride = layer.getFeatureByteSize();
const count = layer.getFeatureCount();
```

然后交由 `StridedColumn` 处理。

---

## 5.2 TypeScript 类型系统

### 目标

建立与 `fastdb4py/type.py` 对应的 TS 字段类型系统。

### 设计

建议用“字段描述对象”而不是单纯字符串，原因是后续需要携带：

- 原始类型枚举
- 是否 numeric
- 对应 TypedArray 构造器
- 归一化字段的 `vmin/vmax`

示例：

```ts
export interface FieldTypeDef {
  kind: 'u8' | 'u16' | 'u32' | 'i32' | 'u8n' | 'u16n' | 'f32' | 'f64' | 'str' | 'wstr' | 'ref' | 'bytes';
  numeric: boolean;
  arrayCtor?: TypedArrayConstructor;
  normalized?: boolean;
}

export const F64: FieldTypeDef = { kind: 'f64', numeric: true, arrayCtor: Float64Array };
export const U32: FieldTypeDef = { kind: 'u32', numeric: true, arrayCtor: Uint32Array };
```

### schema 声明方式

推荐主方案：**静态 schema 定义**

```ts
class Point extends Feature {
  static schema = defineSchema({
    x: F64,
    y: F64,
    z: F64,
  });
}
```

优点：

- 不依赖装饰器
- 更容易分析与缓存
- 更适合运行时 schema introspection

不建议一开始使用装饰器作为主方案，因为：

- 浏览器工具链兼容性不一定统一
- 后续调试 Proxy + decorator 会更复杂

---

## 5.3 Feature 设计

### 目标

复刻 Python `Feature` 的双模式：

1. **Pure TS 模式**
   - 还没绑定到底层 DB
   - 数据存放在 `_cache`

2. **DB-mapped 模式**
   - 绑定到底层 `WxFeature`
   - 读写分发到 WASM 内核

### 设计方案

#### 内部状态

```ts
class Feature {
  _origin: WxFeature | null;
  _db: WxDatabase | WxDatabaseBuild | null;
  _cache: Record<string, unknown> | null;
  _schema: ClassSchema;
}
```

#### 字段访问策略

TS 没有 Python 的 `__getattr__` / `__setattr__`，建议用 `Proxy`：

```ts
function createFeatureProxy<T extends Feature>(feature: T): T {
  return new Proxy(feature, {
    get(target, prop, receiver) {
      if (typeof prop === 'string' && target._schema.fieldMap.has(prop)) {
        return readField(target, prop);
      }
      return Reflect.get(target, prop, receiver);
    },
    set(target, prop, value, receiver) {
      if (typeof prop === 'string' && target._schema.fieldMap.has(prop)) {
        writeField(target, prop, value);
        return true;
      }
      return Reflect.set(target, prop, value, receiver);
    }
  });
}
```

### 额外约束

- Proxy 对象应只在工厂函数中创建，避免用户直接 `new Feature()`
- 要缓存 schema 与 field index，避免每次属性访问都做字符串查找
- TS 层要尽量复用 Python 中已有的“字段索引预计算”思想

---

## 5.4 ORM 设计

### 目标

让浏览器端具备与 Python ORM 相近的生命周期管理能力。

### 核心 API

```ts
class ORM {
  static create(): ORM;
  static truncate(defns: TableDefn[]): ORM;
  static fromBuffer(data: Uint8Array | ArrayBuffer): ORM;

  push(feature: Feature, tableName?: string): void;
  combine(): void;
  toBuffer(): Uint8Array;
  table<T extends Feature>(type: FeatureClass<T>, name?: string): Table<T>;
}
```

### 方法语义

#### `create()`

对应 Python 的动态构建模式。

适合：

- 行数事先未知
- 序列化对象图
- 流式追加

#### `truncate(defns)`

对应 Python 的固定容量表模式。

适合：

- 已知容量
- 数值字段批量写入
- 最快列式写路径

限制应与 Python 对齐：

- 初期不支持 `str` / `wstr` / `bytes` 的 truncate
- `ref` 支持需要所引用表显式存在

#### `fromBuffer(buffer)`

替代 Python 的文件加载。

应用场景：

- 从后端接口拿到 `ArrayBuffer`
- 从 WebSocket / fetch / SharedWorker 获得二进制数据
- 直接在浏览器中映射已有 fastdb buffer

#### `toBuffer()`

替代 Python `save()` 到文件的需求。

应用场景：

- 发回后端
- 放入 IndexedDB / Cache Storage（如果后续需要）
- 作为序列化器中间结果

---

## 5.5 Table 与列访问设计

### 为什么这里是核心

fastdb 的性能优势很大程度来自列访问。

Python 中：

- `table.column.x` 返回 NumPy view
- 零拷贝 + 向量化操作

浏览器中不能直接得到 NumPy，但可以复刻“直接指向底层内存”的思想。

### `StridedColumn` 设计

由于 fastdb 的列并不是严格连续紧凑列式内存，而是依赖：

- base address
- field offset
- feature byte size

所以浏览器端需要一个带 stride 的列包装器。

建议接口：

```ts
class StridedColumn {
  readonly length: number;
  readonly byteOffset: number;
  readonly stride: number;
  readonly elementSize: number;

  get(i: number): number;
  set(i: number, value: number): void;
  fill(values: ArrayLike<number>): void;
  toArray(): TypedArray;
  forEach(fn: (value: number, i: number) => void): void;
}
```

### 为什么不是直接返回 TypedArray

因为标准 `TypedArray` 不支持 stride。

只有在特殊情况下可以返回真正零拷贝 `TypedArray`：

- stride == elementSize

但 fastdb 的常见布局下通常不满足。

因此合理做法是：

- 默认返回 `StridedColumn`
- 内部持有 `DataView` / HEAP buffer
- 必要时提供 `toArray()` 拷贝出连续数组

### Table 行访问

建议同时支持：

- `table.get(i)`
- `[Symbol.iterator]()`
- `iterReuse()`

其中 `iterReuse()` 很重要，因为这对应 Python 的高性能行遍历优化。

---

## 5.6 FastSerializer 设计

### 这是整个 fastdb4ts 的关键部分

原因：

- 用户明确提出优先级很高
- 它直接对应“前后端二进制通信层”
- 它决定 TS/Python 是否真正形成统一生态

### 设计目标

实现与 Python `FastSerializer` **二进制完全兼容**。

### 需要复刻的协议特性

1. scalar fields → columnar storage
2. `List[U32]` / `List[F64]` → 辅助 layer
3. 其他 list / bytes / ref → blob
4. ref 编码：
   - `[layer_idx:u16][feature_idx:u32]`
5. 支持循环引用与 identity 保持

### 实现方式

建议直接按 Python 版思路拆成两部分：

#### Dump 侧

- `_DumpContext`
  - 遍历对象图
  - 为对象分配 `(layer_idx, feature_idx)`
  - 按类型归组

- `dumps(root)`
  - 创建 `ORM.create()`
  - 为每个类型建 layer
  - 为 numeric list 建 auxiliary layer
  - 写入 scalar fields
  - 写入 blob geometry-like data
  - 输出 buffer

#### Load 侧

- `_LoadContext`
  - 先创建所有对象壳
  - 再填 scalar fields
  - 再修复 ref 与循环引用

- `loads(data, rootType)`
  - 调用 `ORM.fromBuffer()`
  - 遍历各 layer
  - 重建对象图

### 为什么必须协议兼容

最终需要支持：

- Python `FastSerializer.dumps()` → TS `loads()`
- TS `dumps()` → Python `loads()`

这才是真正实现前后端统一数据层。

---

## 六、构建与隔离实施方案

## 6.1 对现有 `fastcarto` 的修改范围

应严格压缩到最小。

建议只做以下两项：

1. 修复 `fastcarto/fastdb/CMakeLists.txt` 中 `BUILD_TYPE` 未生效的问题

当前：

```cmake
if(is_emscripten)
    set(BUILD_TYPE STATIC)
else()
    set(BUILD_TYPE SHARED)
endif()

add_library(${PROJECT_NAME} SHARED ${SOURCES})
```

应改为：

```cmake
add_library(${PROJECT_NAME} ${BUILD_TYPE} ${SOURCES})
```

2. 删除/注释无效的历史遗留 `emcc` post-build 片段

因为它依赖不存在的文件，保留只会制造歧义。

除此之外，不应向 `fastcarto` 中引入任何 TS/WASM 专用源文件。

## 6.2 独立构建入口

`ts/build-wasm.sh`

建议流程：

```bash
emcmake cmake -S ts/embind -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm
```

### `ts/embind/CMakeLists.txt`

职责：

- 校验当前工具链确实是 Emscripten
- 以独立 build dir 方式引入 `../../fastcarto`
- 链接 `fastdb` 核心静态库
- 输出 `fastdb4ts.js` + `fastdb4ts.wasm` 到 `ts/fastdb4ts/src/wasm/`

这样可以保证：

- Python 构建完全不受影响
- 只有运行 `build-wasm.sh` 时才会进入 WASM 相关逻辑

---

## 七、分阶段实施计划

## Phase 0：搭建最小可运行 WASM 骨架

### 目标

得到一个浏览器中可加载的 `fastdb4ts.js` + `fastdb4ts.wasm`。

### 工作项

1. 创建 `ts/embind/CMakeLists.txt`
2. 创建 `ts/embind/fastdb4ts.cpp`
3. 创建 `ts/build-wasm.sh`
4. 初始化 `ts/fastdb4ts/package.json`
5. 初始化 `ts/fastdb4ts/tsconfig.json`
6. 在 `src/wasm/` 放置构建产物

### 最小验收标准

```ts
const mod = await FastdbWasm();
const stream = new mod.WxMemoryStream();
stream.delete();
```

---

## Phase 1：完成 Embind 核心绑定

### 目标

从 TS 直接创建/读取 fastdb database。

### 工作项

1. 绑定 `WxDatabaseBuild`
2. 绑定 `WxLayerTableBuild`
3. 绑定 `WxDatabase`
4. 绑定 `WxLayerTable`
5. 绑定 `WxFeature`
6. 提供 buffer / address / field-offset wrapper
7. 提供 batch scalar read/write wrapper

### 验收标准

- TS 能构建一个简单表
- 写入几条数据
- 输出 buffer
- 再从 buffer 加载回来读出字段

---

## Phase 2：完成 TS 类型系统与 Feature

### 目标

让用户可以用 TS 定义数据模型。

### 工作项

1. `types.ts`
2. `schema.ts`
3. `feature.ts`
4. schema cache
5. Proxy dispatch
6. pure TS feature 测试

### 验收标准

```ts
class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64 });
}

const p = createFeature(Point);
p.x = 1.5;
console.log(p.x); // 1.5
```

---

## Phase 3：完成 ORM / Table / Column

### 目标

让 TS 端具备与 fastdb4py 接近的表操作能力。

### 工作项

1. `ORM.create()`
2. `ORM.truncate()`
3. `ORM.fromBuffer()`
4. `ORM.toBuffer()`
5. `Table<T>`
6. `ColumnAccessor`
7. `StridedColumn`
8. `iterReuse()`

### 验收标准

- 创建固定大小表
- 批量写列
- 读回列值
- 行迭代正常

---

## Phase 4：完成 FastSerializer

### 目标

实现前后端统一对象图序列化层。

### 工作项

1. 复刻 `_DumpContext`
2. 复刻 `_LoadContext`
3. 实现 `dumps()`
4. 实现 `loads()`
5. 支持 ref
6. 支持 list
7. 支持 cycle
8. 写跨语言互操作测试

### 验收标准

- Python dump → TS load 成功
- TS dump → Python load 成功
- 循环引用对象图往返成功

---

## Phase 5：工程化与发布

### 目标

把 fastdb4ts 做成可维护、可发布、可集成的前端库。

### 工作项

1. 增加测试脚本
2. 增加 bundler 配置
3. 输出类型声明
4. README
5. 示例项目
6. CI 构建

---

## 八、关键风险与对策

### 风险 1：WASM 指针生命周期管理复杂

问题：

- Embind 对象需要显式 `.delete()`
- 用户直接操作底层对象容易泄漏

对策：

- 底层 WASM 对象尽量只存在于内部封装层
- 对外暴露 TS 高层对象
- 在内部通过 `FinalizationRegistry` 做辅助回收（仅辅助，不依赖）

### 风险 2：列访问无法做到 NumPy 那种天然体验

问题：

- JS 原生没有 stride-aware ndarray

对策：

- 用 `StridedColumn` 复刻常用操作
- 保证零拷贝读取路径可用
- 批量计算场景提供 `toArray()` 导出连续数组

### 风险 3：FastSerializer 协议复刻复杂

问题：

- 涉及对象图遍历、ID 分配、ref 编码、辅助 layer、循环引用修复

对策：

- 严格按 Python 版本结构分阶段翻译
- 每个中间步骤都做 Python/TS fixture 对照测试

### 风险 4：浏览器环境下字符串与宽字符串支持细节复杂

问题：

- `WSTR` 在 WASM/JS UTF 编码转换上需要谨慎处理

对策：

- 第一阶段优先确保 `STR` 与 numeric types
- `WSTR` 单列实现并重点补测试

---

## 九、建议的实施顺序

虽然用户希望优先拿到 serializer，但从工程角度最稳妥顺序是：

1. 先打通 WASM 构建
2. 再暴露 C++ binding
3. 再实现 Feature / ORM / Table 主干
4. 最后实现 FastSerializer

原因是 serializer 本质上依赖：

- schema
- ORM 建表能力
- buffer 输入输出
- ref 处理能力

如果没有这些基础，serializer 会被迫写成大量临时绕路逻辑，后续很难维护。

所以**优先级理解应为：serializer 是核心目标，但不应在基础设施之前孤立实现。**

---

## 十、最终交付目标

完成后，理想使用方式应类似如下：

```ts
import {
  initFastdb,
  Feature,
  ORM,
  TableDefn,
  FastSerializer,
  F64,
  U32,
  defineSchema,
} from 'fastdb4ts';

await initFastdb();

class Point extends Feature {
  static schema = defineSchema({
    id: U32,
    x: F64,
    y: F64,
  });
}

const db = ORM.truncate([
  new TableDefn(Point, 1000),
]);

const table = db.table(Point);
table.column.x.fill(new Float64Array(1000));

const bytes = db.toBuffer();
const db2 = ORM.fromBuffer(bytes);

const p = createFeature(Point);
p.id = 1;
p.x = 3.14;
p.y = 2.71;

const blob = FastSerializer.dumps(p);
const restored = FastSerializer.loads(blob, Point);
```

这个使用模型应当成为 fastdb4ts 的设计准绳。

---

## 十一、结论

fastdb4ts 是可行的，而且现有代码库已经提供了非常好的基础：

- C++ 内核已成熟
- Python binding 已经明确了绑定边界与高层语义
- FastSerializer 协议已有参考实现

最关键的实施策略不是“能不能做”，而是“如何做得干净、可维护、可兼容”。

本方案的核心结论是：

1. **绑定层用 Embind，不用 SWIG**
2. **Embind 完全隔离在 `ts/embind/`**
3. **TS 高层 API 复刻 Python 语义，但按浏览器环境重新设计**
4. **Serializer 协议兼容是整个项目的最高一致性约束**
5. **工程顺序必须先打通 WASM/ORM 主干，再落 Serializer**

如果后续进入实施阶段，应先从 **Phase 0 + Phase 1** 开始，尽快获得一个最小可运行的 WASM 版本，再逐步向完整的 fastdb4ts 演进。

---

## 十二、可直接开工的文件级任务拆分

下面把实施阶段进一步细化到文件级别，目标是让后续开发可以按文件逐项推进，而不是停留在概念层。

## 12.1 Phase 0 文件级任务

### `fastcarto/fastdb/CMakeLists.txt`

任务：

- 将 `add_library(${PROJECT_NAME} SHARED ${SOURCES})` 改为 `add_library(${PROJECT_NAME} ${BUILD_TYPE} ${SOURCES})`
- 删除或注释掉现有失效的 Emscripten post-build 片段
- 保持 Python / Go / Node 现有逻辑不变

验收点：

- 常规 Python 构建路径无行为变化
- 在 Emscripten toolchain 下核心库以 `STATIC` 形式构建

### `ts/embind/CMakeLists.txt`

任务：

- 新建独立 CMake 入口
- 校验 `CMAKE_SYSTEM_NAME == Emscripten`
- 通过 `add_subdirectory()` 引入 `../../fastcarto`
- 创建 `fastdb4ts` executable target
- 设置输出路径到 `ts/fastdb4ts/src/wasm/`
- 添加 `--bind` 与常用 Emscripten 链接参数

验收点：

- `emcmake cmake -S ts/embind -B build-wasm` 成功
- `cmake --build build-wasm` 成功产出 `.js` + `.wasm`

### `ts/embind/fastdb4ts.cpp`

任务：

- 创建最小 Embind 模块
- 先只绑定 `MemoryStream`
- 验证 TS 可 new/delete 一个 C++ 对象

验收点：

- 浏览器或测试环境中 `new mod.WxMemoryStream()` 成功

### `ts/build-wasm.sh`

任务：

- 封装完整 WASM 构建命令
- 支持清理旧 build 目录
- 失败时直接退出

验收点：

- 单命令完成 WASM 构建

### `ts/fastdb4ts/package.json`

任务：

- 初始化 npm 包
- 添加 `build`、`test`、`build:wasm` 脚本
- 配置 `type`, `exports`, `types`

### `ts/fastdb4ts/tsconfig.json`

任务：

- 启用严格 TS 设置
- 输出 declaration
- 支持浏览器 ESM 目标

### `ts/fastdb4ts/src/index.ts`

任务：

- 暂时只导出 `initFastdb` 与最小 wasm loader

### `ts/fastdb4ts/src/wasm-loader.ts`

任务：

- 包装 Emscripten 模块初始化
- 做单例缓存
- 提供 `initFastdb()` 和 `getFastdbModule()`

验收点：

- 重复初始化行为稳定
- 初始化失败有清晰错误

---

## 12.2 Phase 1 文件级任务

### `ts/embind/fastdb4ts.cpp`

任务拆分如下：

#### Part A：基础对象

- 绑定 `chunk_data_t`
- 绑定 `MemoryStream`
- 增加 buffer size / pointer wrapper

#### Part B：Build 侧

- 绑定 `FastVectorDbBuild`
- 绑定 `FastVectorDbLayerBuild`
- 暴露 `begin`, `truncate`, `createLayerBegin`, `createLayerEnd`
- 暴露 `addField`, `setGeometryType`
- 暴露 `addFeatureBegin`, `setField(...)`, `addFeatureEnd`

#### Part C：Read 侧

- 绑定 `FastVectorDb`
- 绑定 `FastVectorDbLayer`
- 绑定 `FastVectorDbFeature`
- 暴露 layer count / layer lookup / feature lookup
- 暴露 `getFieldOffset`, `getFeatureByteSize`, `getFieldCount`, `getFeatureCount`
- 暴露 `getFieldAsInt`, `getFieldAsFloat`, `getFieldAsString`

#### Part D：batch API

- 为 `getFieldsAsDoubles` 提供 JS/WASM 友好 wrapper
- 为 `setFieldsFromDoubles` 提供 JS/WASM 友好 wrapper

#### Part E：辅助 wrapper

- `loadFromHeap(offset, size)`
- `bufferPtr()`
- `bufferSize()`
- `featureAddress()`
- 结构化 field defn 返回

验收点：

- TS 可以完整构建一张简单数值表并读写
- 可以从 buffer 重载数据库

### `ts/fastdb4ts/src/wasm-loader.ts`

任务：

- 补充对 Embind 类型的 TS 包装
- 封装内部 delete 生命周期

### `ts/tests/test-orm.ts`

任务：

- 编写最小集成测试，直接验证 WASM binding 成功

---

## 12.3 Phase 2 文件级任务

### `ts/fastdb4ts/src/types.ts`

任务：

- 定义全部字段类型常量
- 定义 kind → TypedArray / numeric 属性映射
- 定义 `isScalarType`、`isNumericType`

### `ts/fastdb4ts/src/schema.ts`

任务：

- 实现 `defineSchema()`
- 生成字段列表、字段索引、标量字段列表
- 实现 schema cache

建议结构：

- `fieldList`
- `fieldMap`
- `scalarFieldIds`
- `numericFieldIds`
- `refFieldIds`

### `ts/fastdb4ts/src/feature.ts`

任务：

- 定义 `Feature` 基类
- 实现 `fixed` getter
- 实现 `mapFrom()` / `createFeature()` 工厂
- 实现 Proxy 读写分发
- 实现 pure TS 模式
- 实现 db-mapped 模式

### `ts/fastdb4ts/src/errors.ts`

任务：

- 定义绑定错误、schema 错误、runtime 错误类型

### `ts/tests/test-types.ts`

任务：

- 测类型映射与 schema 正确性

### `ts/tests/test-feature.ts`

任务：

- 测 pure TS feature 字段访问
- 测 Proxy dispatch

---

## 12.4 Phase 3 文件级任务

### `ts/fastdb4ts/src/orm.ts`

任务：

- 实现 `ORM.create()`
- 实现 `ORM.truncate()`
- 实现 `ORM.fromBuffer()`
- 实现 `push()`
- 实现 `combine()`
- 实现 `toBuffer()`
- 实现 table cache

### `ts/fastdb4ts/src/table.ts`

任务：

- 实现 `Table<T>`
- 实现 `get(i)`
- 实现 `[Symbol.iterator]()`
- 实现 `iterReuse()`
- 实现 `ColumnAccessor`

### `ts/fastdb4ts/src/column.ts`

任务：

- 实现 `StridedColumn`
- 支持 `get/set/fill/toArray/forEach`
- 针对不同 numeric type 做 typed read/write
- 抽离 element size / DataView 访问逻辑

### `ts/tests/test-orm.ts`

任务：

- 测 `create + push + combine + fromBuffer`
- 测 `truncate`
- 测 table lookup

### `ts/tests/test-column.ts`

任务：

- 测列访问
- 测 strided read/write
- 测批量 fill

建议优先只覆盖：

- `U32`
- `F32`
- `F64`

之后再扩展 `U8/U16/I32/U8N/U16N`

---

## 12.5 Phase 4 文件级任务

### `ts/fastdb4ts/src/serializer.ts`

建议拆成内部私有结构：

- `_DumpContext`
- `_LoadContext`
- `_ObjectHandle`
- `_NumericListLayerInfo`
- `encodeBlobField()`
- `decodeBlobField()`

任务：

- 遍历对象图
- 分配对象 ID
- 建立类型到 layer 的映射
- 写 scalar fields
- 写 numeric list auxiliary layers
- 写 blob payload
- 恢复对象图
- 修复 ref / cycle

### `ts/tests/test-serializer.ts`

任务：

- 测简单对象
- 测嵌套对象
- 测 `List<number>`
- 测 `List<Feature>`
- 测循环引用

### `ts/tests/fixtures/`

任务：

- 保存 Python 端生成的 fixture buffer
- 保存预期对象图描述

如果需要跨语言自动验证，后续可以新增：

- `tests/python/fixtures/` 生成脚本
- TS 测试读取这些 fixture

---

## 12.6 Phase 5 文件级任务

### `ts/fastdb4ts/package.json`

任务：

- 完善发布字段
- 增加浏览器入口与类型入口
- 增加 sideEffects 配置

### `ts/fastdb4ts/README.md`

任务：

- 说明初始化方式
- 说明 schema 定义方式
- 说明 ORM / serializer 用法
- 说明与 Python 协议兼容性

### `ts/tests/`

任务：

- 增加浏览器测试运行入口
- 增加 CI 可执行脚本

### GitHub Actions

未来需新增工作流以覆盖：

- Emscripten build
- TypeScript test
- 可能的 Python/TS 互操作 fixture 生成

---

## 十三、推荐实施顺序（按提交粒度）

为了让每一轮提交都保持可验证，建议按下面的切分推进。

### 提交 1：目录与构建骨架

涉及文件：

- `fastcarto/fastdb/CMakeLists.txt`
- `ts/embind/CMakeLists.txt`
- `ts/embind/fastdb4ts.cpp`
- `ts/build-wasm.sh`
- `ts/fastdb4ts/package.json`
- `ts/fastdb4ts/tsconfig.json`
- `ts/fastdb4ts/src/index.ts`
- `ts/fastdb4ts/src/wasm-loader.ts`

目标：

- 产出最小 wasm

### 提交 2：Build/Read 侧 Embind 完整接通

涉及文件：

- `ts/embind/fastdb4ts.cpp`
- `ts/tests/test-orm.ts`

目标：

- TS 能建表、写值、导出 buffer、再读回

### 提交 3：类型系统与 Feature

涉及文件：

- `ts/fastdb4ts/src/types.ts`
- `ts/fastdb4ts/src/schema.ts`
- `ts/fastdb4ts/src/feature.ts`
- `ts/fastdb4ts/src/errors.ts`
- `ts/tests/test-types.ts`
- `ts/tests/test-feature.ts`

目标：

- 用户能定义 Feature 类并在纯 TS 模式下使用

### 提交 4：ORM / Table / Column

涉及文件：

- `ts/fastdb4ts/src/orm.ts`
- `ts/fastdb4ts/src/table.ts`
- `ts/fastdb4ts/src/column.ts`
- `ts/tests/test-orm.ts`
- `ts/tests/test-column.ts`

目标：

- 形成 fastdb4ts 主干 API

### 提交 5：FastSerializer MVP

涉及文件：

- `ts/fastdb4ts/src/serializer.ts`
- `ts/tests/test-serializer.ts`
- `ts/tests/fixtures/*`

目标：

- 简单对象图跨语言互操作成功

### 提交 6：FastSerializer 完整兼容 + 工程化

涉及文件：

- `ts/fastdb4ts/src/serializer.ts`
- `ts/fastdb4ts/README.md`
- CI/workflow 相关文件

目标：

- 支持 cycle / list / ref 完整协议
- 项目具备可发布性

---

## 十四、建议的最小开工清单

如果下一步就进入实现，建议先只做下面这些文件：

1. `fastcarto/fastdb/CMakeLists.txt`
2. `ts/embind/CMakeLists.txt`
3. `ts/embind/fastdb4ts.cpp`
4. `ts/build-wasm.sh`
5. `ts/fastdb4ts/package.json`
6. `ts/fastdb4ts/tsconfig.json`
7. `ts/fastdb4ts/src/index.ts`
8. `ts/fastdb4ts/src/wasm-loader.ts`

做到这一步后，就能非常快地验证整个技术路线是否成立。
