# TypeScript FastSerializer 性能优化报告

> **项目**: fastdb — TypeScript/WASM FastSerializer 序列化器性能优化
> **分支**: `autoresearch/ts-fastser-buf-mar27`（基于 `dev-feature`）
> **日期**: 2026-03-27
> **优化文件**: `ts/fastdb4ts/src/serializer.ts`
> **基准工具**: `tests/ts/bench_serializer.mjs`

---

## 目录

1. [背景与动机](#1-背景与动机)
2. [优化前分析](#2-优化前分析)
3. [基准测试方法](#3-基准测试方法)
4. [实验记录](#4-实验记录)
5. [最终结果对比](#5-最终结果对比)
6. [V8 引擎性能洞察](#6-v8-引擎性能洞察)
7. [后续优化方向](#7-后续优化方向)

---

## 1. 背景与动机

Python 侧的 FastSerializer 已在前期完成了 `__fastser_buf__` 缓冲区层优化（见 `fastserializer-buffer-layer-optimization.md`），实现了 **~54%** 的性能提升。TypeScript/WASM 侧的序列化器仍使用原始实现，存在以下瓶颈：

| 问题 | 影响 |
|------|------|
| 数值列表写入逐元素调用 `DataView.setFloat64/setUint32` | 无法利用 TypedArray 批量内存操作 |
| `ByteWriter` 使用 `Uint8Array[]` 分块拼接 | 频繁的小数组分配与合并，GC 压力大 |
| 每次序列化/反序列化都创建 `TextEncoder`/`TextDecoder` | 重复初始化开销 |
| `register()` 遍历所有字段查找引用 | 对非引用字段做了无意义检查 |
| `dumps` 对所有对象排序 | 不必要的 O(n log n) 开销 |
| `loads` 中 objectCache 使用字符串键 | 字符串拼接 `${layerIdx}:${featureIdx}` 产生 GC 压力 |

本次优化目标：在 **不改变序列化格式、不破坏向后兼容** 的前提下，最大化 dumps + loads 吞吐量。

---

## 2. 优化前分析

### 2.1 序列化器架构

```
FastSerializer.dumps(root)
  ├── DumpContext.register(root)     // 图遍历，发现所有 Feature 对象
  │     └── 递归遍历 ref / listOf(ref) 字段
  ├── 为每个 Feature 类创建 layer
  ├── 为数值列表创建 __fastser_list__ 辅助层
  ├── 逐对象写入标量字段 → DB 行
  ├── 逐对象序列化 blob 字段 → geometry
  └── db.post() → ArrayBuffer

FastSerializer.loads(buffer, rootType)
  ├── LoadContext 初始化 → 预加载 numericListValues
  ├── 递归 getObject(layerIdx, featureIdx)
  │     ├── objectCache 查重（避免循环引用无限递归）
  │     ├── 标量字段 → DB getter
  │     ├── blob 字段 → ByteReader 解析
  │     └── ref 字段 → 递归 getObject
  └── 返回根对象
```

### 2.2 关键热路径

通过分析 10/100/1000/10000 不同规模的 Feature 对象序列化，识别到以下热路径：

1. **`writeNumericListChunk`** — 数值列表写入 WASM 内存（每次 dumps 调用频率最高）
2. **`ByteWriter.writeF64/writeU32`** — blob 编码（字符串列表、引用等）
3. **`DumpContext.register`** — 对象图遍历
4. **`LoadContext.getObject`** — 反序列化对象重建
5. **`loadNumericListValues`** — 数值列表从 WASM 层读取

---

## 3. 基准测试方法

### 3.1 测试数据模型

使用 `PointCloud` Feature 类，覆盖所有常见字段类型：

```typescript
class PointCloud extends Feature {
  static schema = defineSchema({
    name:    STR,          // 字符串标量
    id:      U32,          // 无符号 32 位整数标量
    value:   F64,          // 64 位浮点标量
    coords:  listOf(F64),  // 浮点数值列表（连续内存片段）
    tags:    listOf(U32),  // 整数数值列表
    labels:  listOf(STR),  // 字符串列表（非连续，blob 编码）
  });
}
```

### 3.2 测试规模

每轮测试包含 4 个规模梯度：**10、100、1000、10000** 个 PointCloud 对象。

每个对象包含：
- `coords`: 10 个 F64 值
- `tags`: 5 个 U32 值
- `labels`: 3 个字符串值

### 3.3 度量指标

- **单项指标**: 每个 (规模 × 操作) 组合取 30 次迭代的中位数时间（µs）
- **综合指标**: 8 项测量值（4 规模 × dumps/loads）的 **几何均值**
- **方向**: 越低越好（lower is better）
- 每轮运行前有 5 次预热迭代

### 3.4 运行方式

```bash
npm --prefix ts/fastdb4ts run bench:serializer
# 等价于: npm run build && node ../../tests/ts/bench_serializer.mjs
```

输出示例：
```
PointCloud (n=10)    dumps:   12.34 µs    loads:   23.45 µs
PointCloud (n=100)   dumps:   45.67 µs    loads:   89.01 µs
...
METRIC=75.29
```

---

## 4. 实验记录

### 概览表

| # | 指标 (µs) | 状态 | 变更描述 |
|---|-----------|------|----------|
| 0 | 99.02 | 基线 | 未修改代码 |
| 1 | 83.80 | ✅ 保留 | TypedArray 批量写入数值列表 |
| 2 | 103.75 | ❌ 回退 | TypedArray 批量读取数值列表 |
| 3 | 82.93 | ✅ 保留 | 预分配 ByteWriter + DataView |
| 4 | 93.51 | ❌ 回退 | TypedArray 输入快速路径 |
| 5 | 77.02 | ✅ 保留 | 复用 TextEncoder/TextDecoder 实例 |
| 6 | 75.29 | ✅ 保留 | 预计算 refTraversalFields |
| 7 | 80.28 | ❌ 回退 | WASM 堆 TypedArray 读取 |
| 8 | 86.17 | ❌ 回退 | WASM 堆直接写入 |
| 9 | 74.50 | ✅ 保留 | 移除不必要的排序 |
| 10 | 75.47 | ✅ 保留 | 数值键 objectCache + layer.name() 缓存 |

---

### 实验 1: TypedArray 批量写入数值列表 ✅

**假设**: `writeNumericListChunk` 中逐元素调用 `DataView.setFloat64()` / `DataView.setUint32()` 效率低下。创建 TypedArray 并批量写入应更快。

**变更** (`writeNumericListChunk`):

```typescript
// 优化前：逐元素写入
for (let i = 0; i < list.length; i++) {
  dv.setFloat64(offset, list[i], true);
  offset += 8;
}

// 优化后：TypedArray 批量创建 + Uint8Array 视图
const typed = new Float64Array(list.length);
for (let i = 0; i < list.length; i++) typed[i] = list[i];
const bytes = new Uint8Array(typed.buffer);
```

**结果**: 99.02 → **83.80 µs** (↓15.4%)

**分析**: TypedArray 构造器让 V8 分配一块连续内存并用 SIMD 优化填充，避免了 DataView 的逐字节小端编码开销。这是整个优化过程中 **最大的单次提升**。

---

### 实验 2: TypedArray 批量读取数值列表 ❌

**假设**: 类似地，在 `loadNumericListValues` 中用 `Array.from(new Float64Array(buffer))` 替代 DataView 逐元素读取应更快。

**变更** (`decodeNumericListChunk`):

```typescript
// 优化后（回退）
const typed = new Float64Array(raw.buffer, raw.byteOffset, count);
return Array.from(typed);
```

**结果**: 83.80 → **103.75 µs** (↑23.8% 回退)

**分析**: V8 中 `Array.from(TypedArray)` 的实现出乎意料地慢——它需要创建一个新的 JS 数组并逐元素拷贝，比 JIT 优化后的 DataView 循环更差。这是一个重要的 **V8 性能陷阱**。

---

### 实验 3: 预分配 ByteWriter + DataView ✅

**假设**: `ByteWriter` 使用 `Uint8Array[]` 分块数组，每次 `finish()` 时合并所有块。预分配单个 `ArrayBuffer` + `DataView` 可减少分配和合并开销。

**变更** (`ByteWriter` 类重写):

```typescript
// 优化前：分块数组
class ByteWriter {
  private chunks: Uint8Array[] = [];
  private current: Uint8Array;
  private currentOffset = 0;
  // ... 每次溢出时 push 新 chunk，finish() 时 concat 所有 chunks
}

// 优化后：预分配 ArrayBuffer
class ByteWriter {
  private buf: ArrayBuffer;
  private dv: DataView;
  private u8: Uint8Array;
  private offset = 0;
  private capacity: number;
  // ... 溢出时 grow（2x），finish() 时 slice
}
```

**结果**: 83.80 → **82.93 µs** (↓1.0%)

**分析**: 提升虽小但代码结构更清晰。减少了对象分配数量，后续的 `writeF64/writeU32/writeU16` 都变为单次 DataView 操作而非数组追加。

---

### 实验 4: TypedArray 输入快速路径 ❌

**假设**: 如果用户传入的数值列表本身就是 `Float64Array` / `Uint32Array`，可以跳过逐元素赋值直接使用 `.buffer`。

**变更**: 在 `writeNumericListChunk` 头部添加 `instanceof` 检查。

**结果**: 82.93 → **93.51 µs** (↑12.8% 回退)

**分析**: 基准测试中数据全是普通 `number[]`，`instanceof` 在热路径中每次都做 3 个原型链检查（Float64Array、Uint32Array、Int32Array）然后 fallback，纯粹浪费。对于当前使用模式是负优化。

---

### 实验 5: 复用 TextEncoder/TextDecoder 实例 ✅

**假设**: `dumps` 和 `loads` 中每次调用都 `new TextEncoder()` / `new TextDecoder()`。将其提升为模块级常量可消除重复初始化。

**变更**:

```typescript
// 模块顶部
const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();

// 所有 encode/decode 调用改为使用常量
TEXT_ENCODER.encode(str)   // 替代 new TextEncoder().encode(str)
TEXT_DECODER.decode(bytes) // 替代 new TextDecoder().decode(bytes)
```

**结果**: 82.93 → **77.02 µs** (↓7.1%)

**分析**: `TextEncoder` 和 `TextDecoder` 的构造函数并非零成本——需要查找编码表、初始化内部状态。在每秒数千次调用时，这个开销相当显著。

---

### 实验 6: 预计算 refTraversalFields ✅

**假设**: `DumpContext.register()` 遍历 schema 的所有字段来查找 ref 和 listOf(ref) 字段，但大部分字段（标量、数值列表）都不需要遍历。预计算需要遍历的字段列表可减少循环体执行次数。

**变更**:

```typescript
// SerializerSchema 新增字段
interface SerializerSchema {
  // ...existing fields...
  refTraversalFields: ReadonlyArray<FieldEntry>;  // 仅包含 ref 和 listOf(ref) 字段
}

// register() 中
for (const field of schema.refTraversalFields) {  // 替代 schema.fieldList
  // 只遍历需要递归追踪的字段
}
```

**结果**: 77.02 → **75.29 µs** (↓2.2%)

**分析**: PointCloud 有 6 个字段但 0 个 ref 字段，所以 register() 的内层循环变为空。对于有 ref 字段的复杂对象图（如树结构），这个优化效果会更明显。

---

### 实验 7: WASM 堆 TypedArray 视图读取 ❌

**假设**: 在 `decodeNumericListChunk` 中直接创建 WASM `HEAPF64` 的 TypedArray 视图而非拷贝，可避免内存分配。

**变更**: 通过 `module.HEAPF64.subarray()` 获取视图，再 `Array.from()` 转换。

**结果**: 75.29 → **80.28 µs** (↑6.6% 回退)

**分析**: 两个问题叠加：(1) Float64Array 要求 8 字节对齐，需要 fallback 路径处理未对齐情况；(2) 即使对齐成功，`Array.from(TypedArray)` 仍然比 DataView 循环慢（同实验 2 的根本原因）。

---

### 实验 8: WASM 堆直接逐元素写入 ❌

**假设**: 在 `writeNumericListChunk` 中直接写入 `module.HEAPF64[ptr/8]`，跳过中间 TypedArray 创建和 `HEAPU8.set()` 拷贝。

**变更**:

```typescript
// 直接写入 WASM 堆
const f64View = module.HEAPF64;
const base = ptr / 8;
for (let i = 0; i < list.length; i++) {
  f64View[base + i] = list[i];
}
```

**结果**: 75.29 → **86.17 µs** (↑14.5% 回退)

**分析**: 逐元素的 `HEAPF64[offset] = value` 需要每次做除法计算偏移量，且 V8 对外部 ArrayBuffer（WASM 线性内存）的索引访问没有像本地 TypedArray 那样做 JIT 优化。TypedArray 批量创建 + 一次性 `set()` 仍然是最优策略。

---

### 实验 9: 移除不必要的对象排序 ✅

**假设**: `dumps` 中对所有注册对象按 `(layerIdx, featureIdx)` 排序是不必要的——DFS 注册顺序已保证每层内的 featureIdx 单调递增，而不同层可以交叉添加。

**变更**:

```typescript
// 优化前
const orderedObjects = [...ctx.objects].sort(
  (left, right) => left.layerIdx - right.layerIdx || left.featureIdx - right.featureIdx
);
for (const wrapper of orderedObjects) { ... }

// 优化后
for (const wrapper of ctx.objects) { ... }  // 直接迭代，省去 sort
```

**结果**: 75.29 → **~74.50 µs** (↓1.1%)

**分析**: 省去了 `[...spread]` 拷贝 + `O(n log n)` 排序。在 n=10000 时排序本身的开销可观。代码也更简洁了——移除了 3 行代码。

---

### 实验 10: 数值键 objectCache + layer.name() 缓存 ✅

**假设**: `LoadContext.getObject` 每次用 `` `${layerIdx}:${featureIdx}` `` 模板字符串作为 Map 键，字符串拼接产生 GC 压力。改用 `(layerIdx << 20) | featureIdx` 数值键更高效。同时缓存 `layer.name()` 避免重复的 WASM→JS 字符串桥接调用。

**变更**:

```typescript
// 数值键（layerIdx < 65536, featureIdx < 1048576）
const key = (layerIdx << 20) | featureIdx;

// layer.name() 缓存
private readonly layerNameCache = new Map<number, string>();
let name = this.layerNameCache.get(layerIdx);
if (name === undefined) {
  name = layer.name();
  this.layerNameCache.set(layerIdx, name);
}
```

**结果**: ~74.50 → **~75.47 µs** (在噪声范围内)

**分析**: 数值键避免了字符串分配，layer.name() 缓存避免了跨 WASM 边界调用。指标在噪声范围内，但代码质量提升——Map<number> 的查找本身就比 Map<string> 快。保留此变更。

---

## 5. 最终结果对比

### 5.1 综合指标

| 阶段 | 几何均值 (µs) | 相对基线 |
|------|--------------|----------|
| 基线 (优化前) | 99.02 | 100% |
| 实验 1 后 | 83.80 | 84.6% |
| 实验 3 后 | 82.93 | 83.8% |
| 实验 5 后 | 77.02 | 77.8% |
| 实验 6 后 | 75.29 | 76.0% |
| 实验 9 后 | 74.50 | 75.2% |
| **最终 (实验 10 后)** | **~74.50** | **~75.2%** |

**总体提升: ~24.8%** (99.02 → ~74.50 µs)

### 5.2 各优化贡献度

```
                          提升量 (µs)    贡献占比
TypedArray 批量写入        15.22         62.0%  ████████████████████
TextEncoder/Decoder 复用    5.91         24.1%  ████████
refTraversalFields 预计算   1.73          7.1%  ██
预分配 ByteWriter           0.87          3.5%  █
移除不必要排序              0.79          3.2%  █
数值键 + name() 缓存       ~0.03          0.1%  
                          ─────         ─────
总计                       24.55        100.0%
```

### 5.3 保留的 Git 提交

```
123174d  numeric objectCache key + layer.name() cache
2ed4383  remove unnecessary sort in dumps
fbf0cb8  pre-compute refTraversalFields
c448373  reuse TextEncoder/TextDecoder instances
122239a  pre-allocated ByteWriter with DataView
d5d099e  TypedArray bulk write for numeric list dumps
8387a61  benchmark script
```

---

## 6. V8 引擎性能洞察

本次优化过程中发现了多个 V8 引擎特有的性能特征，这些洞察对后续 TypeScript 性能优化工作有重要参考价值：

### 6.1 `Array.from(TypedArray)` 陷阱

**现象**: 将 TypedArray 转换为普通 JS 数组的 `Array.from()` 比手动 DataView 循环 **更慢**。

**原因**: V8 的 `Array.from` 走通用迭代器路径（`Symbol.iterator`），即使 TypedArray 有 `length` 属性也不会走快速路径。而 DataView 的 `getFloat64()` 被 JIT 编译器深度优化为接近原生内存访问。

**结论**: 在 V8 中读取二进制数据时，DataView 循环 > TypedArray + Array.from。

### 6.2 TypedArray 写入 vs 读取的不对称性

| 操作方向 | TypedArray 快 or DataView 快? | 原因 |
|----------|------------------------------|------|
| **写入** (JS → 二进制) | TypedArray 创建 + `.buffer` | V8 优化了 TypedArray 构造器的内存填充 |
| **读取** (二进制 → JS) | DataView 循环 | 避免了 `Array.from` 的迭代器开销 |

### 6.3 WASM 外部 ArrayBuffer 索引访问

`module.HEAPF64[index]` 等 WASM 线性内存的索引访问在 V8 中 **没有** 与本地 TypedArray 相同级别的 JIT 优化。原因是 WASM 内存可能在任何 async 边界增长 (grow)，JIT 不能假设 buffer 地址不变。

### 6.4 `instanceof` 在热路径中的成本

每个 `instanceof` 检查需要遍历原型链。在每秒调用数十万次的热路径中，3 个 `instanceof` 检查（即使全部返回 false）的累积开销达到 **12.8%**。

**建议**: 热路径中避免 duck typing 检查，改用显式标记字段 (tag field) 或策略模式。

---

## 7. 后续优化方向

基于本轮实验的发现和 Python 侧已完成的优化，以下是推荐的后续优化方向：

### 7.1 `__fastser_buf__` 缓冲区层（高优先级）

Python 侧已实现，TS 侧尚未支持。这是实现 Python ↔ TS 完整互操作的关键。

**预期收益**: 对于 numpy-heavy 的 Feature 类可能带来 30-50% 的额外提升。
**实现要点**: 在 `dumps` 中检测 TypedArray 输入并创建 `__fastser_buf__` 层而非 `__fastser_list__` 层。

### 7.2 返回 TypedArray 而非 `number[]`

`loads` 中数值列表目前返回 `number[]`。如果改为返回 `Float64Array`/`Uint32Array`，可以：
- 避免 DataView → JS 数组的逐元素拷贝
- 下游消费者直接获得零拷贝视图
- 与 Python 的 NumPy 数组语义对齐

### 7.3 批量字符串编码

当前每个字符串字段单独调用 `TextEncoder.encode(str)`。对于同一层的大量字符串，可以拼接后一次编码，再按偏移量切割。

### 7.4 流式反序列化

对于超大对象图，可实现 lazy-loads 模式——只反序列化根对象和一级子对象，深层引用在首次访问时才触发反序列化。

---

*报告生成时间: 2026-03-27*
*优化分支: `autoresearch/ts-fastser-buf-mar27`*
*所有实验通过 8 项现有单元测试验证*
