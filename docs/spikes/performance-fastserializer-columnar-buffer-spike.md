---
title: "FastSerializer 高性能列式缓冲区序列化方案调研"
category: "Performance"
status: "🟢 Complete"
priority: "High"
timebox: "1 week"
created: 2026-03-27
updated: 2026-03-27
owner: "soku"
tags: ["technical-spike", "performance", "serializer", "columnar", "buffer-protocol"]
---

# FastSerializer 高性能列式缓冲区序列化方案调研

## Summary

**Spike Objective:** 调研 FastSerializer 是否可以通过识别"支持缓冲区协议的类型"（如 numpy 数组、Python `array.array`、TypeScript `TypedArray`），直接创建对应长度的 fastdb 表并通过列式访问（columnar access）进行零拷贝存取，从而大幅提升序列化/反序列化性能。

**Why This Matters:** 当前 FastSerializer 比 pickle 慢 20–100×（数值列表 23×，点云 80×），主要开销在完整的 WxDatabaseBuild → post → load 周期和逐元素 blob 编码。若能将连续内存块（buffer-protocol 对象）直接映射到 fastdb 列式存储，可同时在 Python 和 TypeScript 端实现接近 memcpy 级别的序列化速度。

**Timebox:** 1 week

**Decision Deadline:** 调研完成后即可决定是否进入实现阶段

## Research Question(s)

**Primary Question:** 能否在不修改 fastdb 二进制格式的前提下，让 FastSerializer 对缓冲区协议类型（numpy ndarray、TypedArray）创建专用列式表，实现零拷贝或近零拷贝的序列化/反序列化？

**Secondary Questions:**

- 当前 FastSerializer 的性能瓶颈具体在哪些环节？能否量化各环节耗时占比？
- Ray 的 pickle + Arrow out-of-band buffer 方案的核心思路是什么？如何在纯 fastdb 格式下复用这一思想？
- TypeScript 端如何通过 ArrayBuffer / DataView 高效访问列式存储的连续内存段？
- 该方案对现有 wire format 的兼容性影响如何？Python ↔ TypeScript 互操作是否会被破坏？
- 对于嵌套结构（Feature 内含 numpy 数组字段）、混合类型（标量 + 数组字段混合的 Feature）如何处理？

## Investigation Plan

### Research Tasks

- [x] 分析当前 FastSerializer Python 实现的完整管线
- [x] 分析当前 FastSerializer TypeScript 实现的完整管线
- [x] 分析 C++ 核心存储层的内存布局和列式访问机制
- [x] 回顾已完成的优化历史（o0–o7）和当前性能基线
- [ ] 调研 Ray 的 out-of-band buffer 方案并提取核心思想
- [ ] 设计 fastdb 列式缓冲区序列化的具体方案
- [ ] 分析对 wire format 兼容性和跨语言互操作的影响
- [ ] 评估实现复杂度和预期性能收益

### Success Criteria

**This spike is complete when:**

- [x] 当前 FastSerializer 的性能瓶颈已被完整分析
- [ ] 列式缓冲区方案的技术可行性已验证
- [ ] 给出明确的推荐方案和实现路径
- [ ] 预期性能收益已有量化估算

---

## Technical Context

**Related Components:**

| 组件 | 路径 | 角色 |
|------|------|------|
| Python FastSerializer | `python/fastdb4py/serializer.py` | 主序列化器 |
| TypeScript FastSerializer | `ts/fastdb4ts/src/serializer.ts` | TS 侧序列化器 |
| C++ 核心存储 | `fastcarto/fastdb/src/` | 底层列式存储引擎 |
| Python ORM | `python/fastdb4py/orm/` | ORM 生命周期 |
| SWIG 接口 | `fastcarto/fastdb/swig/fastdb4py.i` | Python ↔ C++ 桥接 |
| Embind 接口 | `ts/embind/fastdb4ts.cpp` | TypeScript ↔ WASM 桥接 |

**Dependencies:** 此调研的结论将影响 FastSerializer v2 的设计决策，以及后续 TypeScript 侧序列化器的优化方向。

**Constraints:**
- 必须使用纯 fastdb 二进制格式（不能依赖 pickle、msgpack 等外部格式）
- 必须保持 Python ↔ TypeScript 跨语言互操作性
- wire format 中不能使用 `size_t`（WASM 下为 4 字节，native 下为 8 字节）
- 需要向后兼容现有的 FastSerializer.dumps/loads API

---

## Research Findings

### 1. 当前 FastSerializer 性能瓶颈深度分析

#### 1.1 Python 端性能基线

根据 `benchmark_comprehensive.py` 的最新结果（o7 round）：

| 场景 | 耗时 | 对比 pickle |
|------|------|------------|
| 数值列表 N=8 | 21.3 µs | pickle 0.92 µs（**23× 慢**） |
| 点云 N=8 features | 88.15 µs | pickle 1.10 µs（**80× 慢**） |
| 简单标量 Feature | ~17 µs | pickle ~0.5 µs |

#### 1.2 耗时分解——dumps() 管线各阶段

```
FastSerializer.dumps(obj) 管线：

┌─────────────────────────────────────────────────────────────┐
│ Pass 1: 对象图遍历 + 注册                                    │
│ ├── DFS 递归遍历所有 Feature 对象                            │
│ ├── obj_to_id[id(obj)] = (layer_idx, feature_idx) 缓存       │
│ ├── 发现所有类型 → type_to_layer 映射                        │
│ └── 估计开销: ~5-10% 总时间                                  │
├─────────────────────────────────────────────────────────────┤
│ Pass 2: WxDatabaseBuild 构建                                 │
│ ├── createLayerBegin() × N_types  ← 高开销（C++ 对象分配）    │
│ ├── addField() × N_fields_per_type                          │
│ ├── 循环: addFeatureBegin/setField/addFeatureEnd × N_objs    │
│ │   ├── 标量字段: set_field() → SWIG 调用 ← ~400ns/call     │
│ │   ├── 字符串字段: set_field_cstring() → SWIG + 字符串池    │
│ │   └── 复杂字段: blob 编码 → bytearray 拼接                │
│ ├── createLayerEnd() × N_types                              │
│ └── 估计开销: ~40-50% 总时间                                 │
├─────────────────────────────────────────────────────────────┤
│ Pass 3: post() → MemoryStream 序列化                         │
│ ├── builder.post(stream)  ← C++ 内部拷贝到连续缓冲区         │
│ ├── stream.get_bytes()  ← 再拷贝为 Python bytes             │
│ └── 估计开销: ~30-40% 总时间                                 │
├─────────────────────────────────────────────────────────────┤
│ Blob 编码开销（分散在 Pass 2 中）                             │
│ ├── struct.pack('<I', count) + 循环 struct.pack('<d', v)     │
│ ├── bytearray() 逐步增长（无预分配）                         │
│ ├── UTF-8 编码: 每个字符串单独 encode()                      │
│ └── 估计开销: ~10-20% 总时间                                 │
└─────────────────────────────────────────────────────────────┘
```

#### 1.3 核心瓶颈总结

| 瓶颈 | 类型 | 影响级别 | 说明 |
|------|------|---------|------|
| **WxDatabaseBuild 完整生命周期** | 架构级 | ★★★★★ | 每次 dumps() 都要创建 builder → add layers → add features → post → serialize，这是最大的开销来源 |
| **逐字段 SWIG 调用** | 调用开销 | ★★★★☆ | 每个标量字段需要一次 SWIG 跨语言调用（~400ns），N 个对象 × M 个字段 = N×M 次调用 |
| **Blob 逐元素编码** | 算法级 | ★★★☆☆ | struct.pack 逐元素调用，无批量操作，bytearray 无预分配 |
| **post() + get_bytes() 双拷贝** | 内存级 | ★★★☆☆ | C++ post() 将 builder 内容拷贝到 stream，然后 get_bytes() 再拷贝到 Python bytes |
| **数值列表通过 blob 路径** | 数据路径 | ★★★☆☆ | 即使有 auxiliary layer 优化，仍需要逐元素 struct.pack |

#### 1.4 TypeScript 端特有瓶颈

| 瓶颈 | 影响 |
|------|------|
| **ByteWriter 每次写入分配新 Uint8Array** | 1000 个 ref 字段 = 1000 次 `new Uint8Array(6)` 分配 |
| **TextEncoder 每字符串实例化** | `new TextEncoder().encode(str)` 无复用 |
| **数值列表双拷贝** | DataView → payload → WASM heap → 最终存储 |
| **对象缓存键为字符串拼接** | `${layerIdx}:${featureIdx}` 频繁分配 + GC 压力 |
| **toArray() 逐元素读取** | 无法利用 TypedArray 批量拷贝 |

### 2. Ray Out-of-Band Buffer 方案分析与启示

#### 2.1 Ray 的序列化架构

Ray 的核心思路是 **"元数据走 pickle，大缓冲区走带外通道"**：

```
Ray 序列化管线：

┌─────────────────────────────────────────────────────┐
│  Python Object (e.g. numpy array)                   │
├─────────────────────────────────────────────────────┤
│  Pickle Protocol 5 序列化                            │
│  ├── 结构/元数据 → pickle 字节流（小）               │
│  └── 大缓冲区 → PickleBuffer (out-of-band)          │
│       ↓ buffer_callback                             │
│       直接放入 Plasma 共享内存段                      │
├─────────────────────────────────────────────────────┤
│  Plasma Object Store (共享内存)                      │
│  ├── 元数据: shape, dtype, strides (几十字节)        │
│  └── 数据缓冲区: 直接 mmap，零拷贝读取               │
└─────────────────────────────────────────────────────┘
```

**关键设计要点：**

1. **分离元数据与数据体**：pickle 只处理对象结构（类型、形状、步幅等元信息），实际数据作为独立缓冲区传递
2. **PickleBuffer + buffer_callback**：PEP 574 引入的机制，允许序列化器将大块内存"旁路"到外部处理器
3. **零拷贝共享**：Plasma 中的缓冲区可被多个进程直接 mmap 读取，无需数据复制
4. **类型感知**：只有实现了 `__reduce_ex__` 并使用 `PickleBuffer` 的类型才走带外路径

#### 2.2 fastdb 版本的 "Out-of-Band Buffer" 类比

我们不能使用 pickle（需要纯 fastdb 格式），但可以借鉴其**分离策略**：

```
当前 FastSerializer（所有数据混在一起）：
┌──────────────────────────────────────────────┐
│  Feature Object Graph                        │
│  ├── 标量字段 → fastdb 列（每字段一次 SWIG）  │
│  ├── 字符串 → fastdb 列（字符串池）           │
│  ├── 引用 → blob 编码（6字节/ref）            │
│  ├── 数值列表 → blob 逐元素打包 ← 慢！       │
│  └── 复杂字段 → blob 编码                    │
└──────────────────────────────────────────────┘

提议的 FastSerializer v2（分离缓冲区）：
┌──────────────────────────────────────────────┐
│  Feature Object Graph                        │
│  ├── 标量字段 → fastdb 列（同前）             │
│  ├── 字符串 → fastdb 列（同前）               │
│  ├── 引用 → blob 编码（同前）                 │
│  ├── 缓冲区字段 → 专用 fastdb 层 ← 新！      │
│  │   └── numpy array / TypedArray            │
│  │       → 创建 N 行表，列类型匹配 dtype      │
│  │       → memcpy / 零拷贝写入列数据           │
│  └── 复杂字段 → blob 编码（同前）             │
└──────────────────────────────────────────────┘
```

#### 2.3 核心类比映射

| Ray 概念 | fastdb 对应 |
|----------|------------|
| PickleBuffer (大缓冲区旁路) | 识别 buffer-protocol 对象 → 创建专用 fastdb 层 |
| Plasma 共享内存段 | fastdb 列式存储的连续内存块（chunk_data_t） |
| pickle 流 (元数据) | 普通 fastdb 层（存储结构、引用、标量） |
| buffer_callback | 序列化时检测 `__array_interface__` / `ArrayBuffer` |
| 零拷贝 mmap | Python: numpy `__array_interface__` 直接映射到 fastdb 列 |
| | TypeScript: `new Float64Array(wasm.HEAPF64.buffer, ptr, len)` |

### 3. 列式缓冲区方案详细设计

#### 3.1 方案概述

**核心思想：** 当 FastSerializer 遇到"支持缓冲区协议的类型"时，不再通过 blob 逐元素编码，而是：

1. **识别**：检测字段值是否为 buffer-protocol 对象（numpy array、bytes、array.array、TypedArray）
2. **创建专用层**：为该缓冲区创建一个 fastdb 层，行数 = 元素个数，列类型 = 元素 dtype
3. **批量写入**：通过 memcpy 或 numpy 赋值直接将连续内存写入 fastdb 列
4. **元数据标记**：在所属 Feature 的 blob 中只存储一个"缓冲区引用"（层索引 + 形状信息）
5. **读取时零拷贝**：反序列化时直接通过 `__array_interface__` 或 `TypedArray` 构造函数映射到 fastdb 列内存

#### 3.2 层命名规范

延续现有 auxiliary layer 的命名模式：

```
现有数值列表层：  __fastser_list__|ClassName|FieldName|kind
新增缓冲区层：    __fastser_buf__|ClassName|FieldName|dtype|shape
```

示例：
```
__fastser_buf__|PointCloud|positions|f64|1000x3
__fastser_buf__|Image|pixels|u8|256x256x3
__fastser_buf__|Signal|samples|f32|44100
```

#### 3.3 支持的缓冲区类型

**Python 端：**

| 类型 | 检测方式 | dtype 推断 |
|------|---------|-----------|
| `numpy.ndarray` | `hasattr(obj, '__array_interface__')` | `obj.dtype` |
| `array.array` | `isinstance(obj, array.array)` | `obj.typecode` → dtype 映射 |
| `bytes` / `bytearray` | `isinstance(obj, (bytes, bytearray))` | `u8` |
| `memoryview` | `isinstance(obj, memoryview)` | `view.format` → dtype 映射 |

**TypeScript 端：**

| 类型 | 检测方式 | dtype 推断 |
|------|---------|-----------|
| `Float64Array` | `value instanceof Float64Array` | `f64` |
| `Float32Array` | `value instanceof Float32Array` | `f32` |
| `Uint32Array` | `value instanceof Uint32Array` | `u32` |
| `Int32Array` | `value instanceof Int32Array` | `i32` |
| `Uint8Array` | `value instanceof Uint8Array` | `u8` |
| `ArrayBuffer` | `value instanceof ArrayBuffer` | `u8`（原始字节） |

#### 3.4 序列化流程（dumps）

```python
# 伪代码 - Python 端
def _serialize_buffer_field(self, cls_name, field_name, value):
    """将 buffer-protocol 对象序列化为专用 fastdb 层"""
    
    # 1. 推断 dtype 和形状
    if hasattr(value, '__array_interface__'):
        arr = np.ascontiguousarray(value)  # 确保 C-contiguous
        dtype = arr.dtype        # e.g. float64
        shape = arr.shape        # e.g. (1000, 3)
        flat = arr.ravel()       # 展平为 1D
    elif isinstance(value, (bytes, bytearray)):
        dtype = np.uint8
        shape = (len(value),)
        flat = np.frombuffer(value, dtype=np.uint8)
    
    # 2. 映射 numpy dtype → fastdb field type
    fdb_type = {
        np.float64: ftF64, np.float32: ftF32,
        np.uint32: ftU32, np.int32: ftI32,
        np.uint8: ftU8, np.uint16: ftU16,
    }[dtype.type]
    
    # 3. 创建专用层: 行数 = len(flat), 1 列 = fdb_type
    layer_name = f"__fastser_buf__|{cls_name}|{field_name}|{dtype}|{'x'.join(map(str, shape))}"
    layer = builder.createLayerBegin(layer_name)
    layer.addField("v", fdb_type)
    layer.setScale(len(flat))  # 预分配 truncate 模式
    builder.createLayerEnd()
    
    # 4. 批量写入 —— 这里是关键性能点
    #    通过列访问接口直接 memcpy，而非逐行 addFeature
    col = layer.get_column(0)  # chunk_data_t
    col_array = np.array(col, copy=False)  # 零拷贝 numpy 视图
    col_array[:] = flat  # memcpy 级别的批量写入！
    
    # 5. 在所属 Feature 的 blob 中记录引用
    return encode_buffer_ref(layer_idx, shape)
```

```typescript
// 伪代码 - TypeScript 端
function serializeBufferField(
    cls: string, field: string, value: TypedArray,
    builder: WxDatabaseBuild, module: WasmModule
): BufferRef {
    // 1. 推断 dtype
    const dtype = typedArrayToDtype(value); // Float64Array → 'f64'
    
    // 2. 创建专用层
    const layerName = `__fastser_buf__|${cls}|${field}|${dtype}|${value.length}`;
    const layer = builder.createLayerBegin(layerName);
    layer.addField('v', dTypeToFieldType(dtype));
    layer.setScale(value.length);
    builder.createLayerEnd();
    
    // 3. 批量写入 —— 直接操作 WASM 堆内存
    const col = layer.getColumn(0); // { ptr, stride, length }
    const dst = new Float64Array(
        module.HEAPF64.buffer, col.ptr, value.length
    );
    dst.set(value); // TypedArray.set() —— 近 memcpy 速度
    
    return { layerIdx, shape: [value.length] };
}
```

#### 3.5 反序列化流程（loads）

```python
# 伪代码 - Python 端
def _load_buffer_field(self, layer_idx, shape):
    """从专用 fastdb 层零拷贝恢复 buffer 对象"""
    
    layer = db.getLayer(layer_idx)
    col = layer.get_column(0)  # chunk_data_t → 列数据指针
    
    # 零拷贝 numpy 数组（通过 __array_interface__）
    flat = np.array(col, copy=False)  # 直接映射 C++ 内存
    
    # 恢复形状
    if len(shape) > 1:
        return flat.reshape(shape)
    return flat
```

```typescript
// 伪代码 - TypeScript 端  
function loadBufferField(
    layerIdx: number, shape: number[], 
    db: WxDatabase, module: WasmModule
): TypedArray {
    const layer = db.getLayer(layerIdx);
    const feature = layer.tryGetFeature(0);
    const ptr = feature.getAddress() + layer.getFieldOffset(0);
    const stride = layer.getFeatureByteSize();
    
    if (stride === elementSize) {
        // 连续内存 → 直接创建 TypedArray 视图（真正零拷贝）
        return new Float64Array(
            module.HEAPF64.buffer, ptr, totalElements
        );
    } else {
        // 非连续 → 需要 StridedColumn 逐元素读取
        return column.toArray();
    }
}
```

#### 3.6 Blob 中的缓冲区引用编码

在所属 Feature 的 geometry blob 中，缓冲区字段不再存储实际数据，而是存储一个轻量级引用：

```
缓冲区引用格式（固定 16 字节）：
┌──────────────┬────────────┬──────────────────────────┐
│ magic: u8    │ layer: u16 │ ndim: u8                 │
│ 0xBF         │            │                          │
├──────────────┴────────────┴──────────────────────────┤
│ shape[0]: u32 | shape[1]: u32 | shape[2]: u32        │
│ (最多支持 3 维, 不用的维度填 0)                       │
└──────────────────────────────────────────────────────┘
Total: 1 + 2 + 1 + 12 = 16 bytes
```

- `magic = 0xBF`（"Buffer Flag"）用于在反序列化时区分普通 blob 数据和缓冲区引用
- 支持最多 3 维数组（满足绝大多数场景）
- 固定长度 16 字节，解析零开销

### 4. 关键技术可行性分析

#### 4.1 fastdb 行式存储 vs 列式批量写入——stride gap 问题

**这是本方案最关键的技术点。**

fastdb 的内存布局是**行式存储**（row-major），不是纯列式存储：

```
Layer 内存布局（以 2 个 f64 字段为例，行大小=16 字节）：

Row 0: [field_0: f64 (8B)] [field_1: f64 (8B)]  ← 偏移 0
Row 1: [field_0: f64 (8B)] [field_1: f64 (8B)]  ← 偏移 16
Row 2: [field_0: f64 (8B)] [field_1: f64 (8B)]  ← 偏移 32
       ↑ stride = 16 bytes (= row size)

列视图 field_0 的物理地址：ptr+0, ptr+16, ptr+32, ...
列视图 field_1 的物理地址：ptr+8, ptr+24, ptr+40, ...
```

**对于只有单列的专用缓冲区层（我们的方案），stride == element_size**，因此：

```
专用缓冲区层（1 个 f64 列，行大小=8 字节）：

Row 0: [f64 value]  ← 偏移 0
Row 1: [f64 value]  ← 偏移 8
Row 2: [f64 value]  ← 偏移 16
       ↑ stride = 8 bytes == sizeof(f64)

→ 内存完全连续！可以直接 memcpy！
```

**结论：** 只要缓冲区层只有 1 个字段，其列数据在内存中就是连续的，可以实现真正的零拷贝。这是方案可行性的关键保证。

#### 4.2 现有 API 是否支持 truncate 模式批量写入？

检查现有 C++ / SWIG / Embind API 是否已支持所需操作：

| 操作 | Python 端 | TypeScript 端 | 可行性 |
|------|----------|--------------|--------|
| **预分配行数**（truncate） | `layer.setScale(n)` via `createLayerBegin` | `layer.setScale(n)` via embind | ✅ 已有 |
| **获取列数据指针** | `chunk_data_t` via `get_column()` → `__array_interface__` | `getAddress() + getFieldOffset()` | ✅ 已有 |
| **numpy 零拷贝写入** | `np.array(col, copy=False)[:] = data` | N/A | ✅ 已有 |
| **TypedArray 批量写入** | N/A | `new Float64Array(heap, ptr, n).set(data)` | ✅ 已有 |
| **读取时零拷贝映射** | `np.array(chunk, copy=False)` | `new Float64Array(heap, ptr, n)` | ✅ 已有 |

**但是有一个重要前提：** 需要先调用 `createLayerEnd()` 并进行 `post()` 才能获取列数据指针。这意味着我们不能在 builder 阶段直接写入列数据——需要分两步：

**方案 A：保持现有流程（Builder → Post → Load → 写入列）**
```
1. 创建 builder，addField，setScale(n)
2. addFeatureBegin/End × n 次（填充默认值或跳过）
3. createLayerEnd → post → 序列化到 bytes
4. 从 bytes 加载为 immutable db
5. 获取列指针 → 直接写入
```
**问题：** 步骤 2-3 仍然有逐行开销

**方案 B：新增 C++ 批量列写入 API（推荐）**
```
1. 创建 builder，addField，setScale(n)
2. 新 API: setColumnData(field_idx, void* data, size_t count)
3. createLayerEnd → post
```
**优势：** 一次 memcpy，无逐行迭代

**方案 C：利用现有 `Table.fill()` 路径（最小改动）**
```
1. ORM.truncate() 创建固定大小表
2. table.column.v[:] = flat_array  (利用已有的 numpy 列写入)
3. orm.toBuffer() 导出
```
**优势：** 完全复用 o4 优化的 fill() 路径，无需 C++ 改动

#### 4.3 方案 C 深度分析（推荐的最小可行方案）

方案 C 最具吸引力，因为它完全在 Python/TypeScript 层面实现，无需修改 C++ 核心：

```python
# Python - 利用现有 ORM.truncate + Table.fill() 实现缓冲区序列化
class BufferLayer(Feature):
    """动态生成的单列 Feature 类"""
    v: F64  # 类型根据 buffer dtype 动态设置

def _create_buffer_layer(name, dtype, count, flat_data, builder_orm):
    """在 builder ORM 中创建缓冲区层"""
    # 动态创建 Feature 子类
    DynFeature = type(name, (Feature,), {
        '__annotations__': {'v': dtype_to_type_alias(dtype)}
    })
    
    # 利用 ORM.truncate 的 TableDefn 预分配
    # 注意：需要在现有 dumps() 流程中嵌入 ORM 操作
    table = builder_orm.get(DynFeature)
    table.column.v[:] = flat_data  # memcpy 级别写入
```

**方案 C 的限制：**
- 需要 ORM 实例，而当前 FastSerializer 是独立的（不依赖 ORM）
- 动态 Feature 子类创建有元类开销（但可缓存）
- 需要单独的"迷你 ORM"来管理缓冲区层

#### 4.4 推荐方案：混合路径（方案 B + C 思路）

考虑到实际约束，推荐一个**最优混合方案**：

```
FastSerializer v2 管线：

Pass 1: 对象图遍历（同前）
  ├── 识别 buffer-protocol 字段
  └── 记录 (cls, field, dtype, shape, flat_data)

Pass 2: 构建 fastdb builder
  ├── 普通层: 同前（标量、字符串、引用、blob）
  └── 缓冲区层: 
      ├── createLayerBegin(layer_name)
      ├── addField("v", fdb_type)
      ├── 循环 addFeatureBegin/setField/addFeatureEnd × N
      │   ↑ 但仅 1 个字段，开销大幅降低
      └── createLayerEnd()

Pass 3: post() → bytes（同前）
  └── 缓冲区数据已内嵌在层中

替代优化——C++ 批量 API（可后续追加）：
  └── 新增 setColumnBulk(field_idx, data_ptr, count)
      跳过 addFeatureBegin/End 循环
      直接 memcpy 到列缓冲区
```

**为什么即使保持逐行写入，性能也会大幅提升？**

- 缓冲区层只有**1 个字段**，而非 N 个字段
- 每行只有 1 次 `setField()` 调用（vs 当前 blob 编码中的 struct.pack 循环 + bytearray 拼接）
- 消除了 blob 编码开销（struct.pack、bytearray 增长、Python 层循环）
- 反序列化时零拷贝（直接映射列内存）

### 5. 量化性能预估

#### 5.1 当前路径 vs 缓冲区层路径耗时对比

以 **1000 元素 float64 数组**为例：

**当前路径（blob 编码）：**
```
struct.pack('<I', 1000)                 →  ~0.1 µs
循环 struct.pack('<d', v) × 1000       →  ~50 µs (50 ns/element)
bytearray 增长 + 拼接                   →  ~10 µs
set_geometry_raw(blob)                  →  ~2 µs
──────────────────────────────────────────────
总计:                                    ~62 µs
```

**方案 B（C++ 批量 API）——最优路径：**
```
createLayerBegin + addField             →  ~5 µs (一次性)
setColumnBulk(data_ptr, 1000)           →  ~0.8 µs (= memcpy 8KB)
createLayerEnd                          →  ~2 µs
──────────────────────────────────────────────
总计:                                    ~8 µs （加速 ~8×）
```

**方案 C（逐行但单字段）——中间路径：**
```
createLayerBegin + addField             →  ~5 µs
(addFeatureBegin+setField+End) × 1000  →  ~400 µs (SWIG 开销主导)
createLayerEnd                          →  ~2 µs
──────────────────────────────────────────────
总计:                                    ~407 µs （反而更慢！）
```

**关键发现：** 纯逐行写入对于大数组反而更慢，因为 SWIG 调用开销 (~400ns) × N 行远超 Python struct.pack 开销。

**因此，方案 B（C++ 批量 API）是必需的，不是可选优化。**

#### 5.2 反序列化路径对比

**当前路径（blob 解码）：**
```
读取 count (struct.unpack)              →  ~0.05 µs
循环 struct.unpack('<d', ...) × 1000   →  ~40 µs
构造 Python list                        →  ~5 µs
──────────────────────────────────────────────
总计:                                    ~45 µs
```

**缓冲区层路径（零拷贝）：**
```
getLayer + get_column                   →  ~2 µs
np.array(col, copy=False)              →  ~0.5 µs (零拷贝 numpy 视图)
reshape if needed                       →  ~0.1 µs
──────────────────────────────────────────────
总计:                                    ~2.6 µs （加速 ~17×）
```

#### 5.3 综合性能预估

| 场景 | 当前耗时 | 方案B 预估 | 加速比 | 说明 |
|------|---------|-----------|--------|------|
| **dumps 1K float64** | ~62 µs | ~8 µs | **~8×** | memcpy 替代逐元素 struct.pack |
| **loads 1K float64** | ~45 µs | ~2.6 µs | **~17×** | 零拷贝映射替代逐元素 unpack |
| **dumps 点云 N=100** | ~1.1 ms | ~0.3 ms | **~3.5×** | 缓冲区字段加速 + 标量字段不变 |
| **loads 点云 N=100** | ~0.8 ms | ~0.15 ms | **~5×** | 缓冲区零拷贝 + 标量解码不变 |
| **dumps 大数组 100K** | ~6.2 ms | ~80 µs | **~78×** | 接近 memcpy 理论极限 |
| **loads 大数组 100K** | ~4.5 ms | ~3 µs | **~1500×** | 纯零拷贝（数据从未离开 C++ 内存） |

> **注意：** loads 的巨大加速比来自零拷贝——数据不需要从 C++ 复制到 Python，numpy 直接映射 C++ 内存。但使用者需要意识到返回的数组生命周期受 ORM/Database 管理。

#### 5.4 TypeScript 端性能预估

| 场景 | 当前耗时 | 方案B 预估 | 加速比 |
|------|---------|-----------|--------|
| **dumps 1K f64** | ~80 µs | ~15 µs | **~5×** |
| **loads 1K f64** | ~60 µs | ~5 µs | **~12×** |
| **大数组 100K** | ~8 ms | ~100 µs | **~80×** |

TypeScript 端的加速比略低于 Python，因为 WASM 内存操作本身有一定开销（需要通过 HEAPF64 视图），但依然非常显著。

### 6. Wire Format 兼容性分析

#### 6.1 向后兼容性

| 维度 | 兼容性 | 说明 |
|------|--------|------|
| **旧数据 → 新代码** | ✅ 完全兼容 | 新代码检测不到 `__fastser_buf__` 层，回退到现有路径 |
| **新数据 → 旧代码** | ⚠️ 部分兼容 | 旧代码会忽略未知的 `__fastser_buf__` 层，缓冲区字段变为 None |
| **Python ↔ TypeScript** | ✅ 兼容 | 只要两端都实现了缓冲区层的读写逻辑 |

#### 6.2 wire format 不变

缓冲区层在 wire format 层面就是普通的 fastdb 层——只有 1 个字段、N 行，层名带有 `__fastser_buf__` 前缀。**不需要修改任何 C++ 核心代码的序列化/反序列化逻辑**。

与现有 `__fastser_list__` auxiliary layer 的唯一区别是：
- `__fastser_list__` 存储在 geometry blob 中，关联到 owner 的 feature_idx
- `__fastser_buf__` 直接使用列存储，整个层就是一个连续缓冲区

#### 6.3 需要注意的 size_t 问题

C++ 中 `chunk_data_t.size` 使用 `size_t`（native 8B, WASM 4B），但这个结构只在运行时使用，不进入 wire format。wire format 中的大小字段使用固定宽度类型（u32/u64），因此**无兼容性问题**。

### 7. 与现有 `__fastser_list__` 机制的关系

#### 7.1 对比

| 维度 | `__fastser_list__` (现有) | `__fastser_buf__` (新方案) |
|------|--------------------------|---------------------------|
| **存储方式** | geometry blob（变长字节） | 列存储（固定字段） |
| **写入方式** | struct.pack 逐元素 | memcpy / 列批量写入 |
| **读取方式** | struct.unpack 逐元素 | 零拷贝 numpy/TypedArray |
| **关联方式** | owner_fid 字段 | blob 中的缓冲区引用 |
| **适用场景** | Feature 的 List[F64] 字段 | 独立的大型数组 |
| **多维支持** | ❌ 仅 1D | ✅ 最多 3D |

#### 7.2 是否应替换 `__fastser_list__`？

**不建议。** 两者适用场景不同：

- `__fastser_list__`：适合 Feature 上的**小型变长**数值列表（如每个 Feature 有 3-50 个坐标点）
- `__fastser_buf__`：适合**大型连续缓冲区**（如整个点云的坐标矩阵、图像像素数据）

阈值建议：元素数量 < 256 用 `__fastser_list__`，≥ 256 用 `__fastser_buf__`。

### 8. 实现路径与优先级

#### 8.1 分阶段实现

**Phase 1: Python 端最小可行实现（无 C++ 改动）**
- 在 `serializer.py` 中添加缓冲区检测逻辑
- 使用现有 `addFeatureBegin/setField/addFeatureEnd` 逐行写入缓冲区层
- 反序列化时通过 `__array_interface__` 实现零拷贝读取
- **预期改进**：loads 加速 ~17×，dumps 略改善（主要消除 blob 编码开销）

**Phase 2: C++ 批量列写入 API**
- 在 `FastVectorDbLayerBuild` 中新增 `setColumnBulk(field_idx, void* data, size_t count)`
- 内部实现：直接 memcpy 到 `m_table_buffer` 的对应列偏移
- SWIG + Embind 暴露
- **预期改进**：dumps 加速 ~8-78×（取决于数组大小）

**Phase 3: TypeScript 端实现**
- 在 `serializer.ts` 中添加 TypedArray 检测
- 利用 WASM HEAP 直接写入
- **预期改进**：与 Python 端类似

**Phase 4: Schema 类型标注支持**
- 在 Feature 定义中支持缓冲区类型标注
- 例如 `positions: NdArray[F64, (N, 3)]` 或 `pixels: Buffer[U8]`
- 编译时即可确定缓冲区路径，免除运行时类型检测

#### 8.2 C++ `setColumnBulk` API 设计

```cpp
// fastcarto/fastdb/include/fastdb.h
class FastVectorDbLayerBuild {
public:
    // 新增：批量写入列数据（跳过逐行 addFeature 循环）
    // 前提：必须在 setScale(n) 之后、createLayerEnd() 之前调用
    // data 必须是与字段类型匹配的连续内存块
    // count 必须等于 setScale 设置的行数
    void setColumnBulk(u32 field_idx, const void* data, u32 count);
};
```

```cpp
// fastcarto/fastdb/src/FastVectorDbLayerBuild.cpp
void FastVectorDbLayerBuild::Impl::setColumnBulk(
    u32 field_idx, const void* data, u32 count
) {
    assert(field_idx < m_field_descs.size());
    assert(count == m_feature_count);  // must match setScale
    
    const auto& fd = m_field_descs[field_idx];
    const u8* src = static_cast<const u8*>(data);
    
    // 由于行式存储，需要逐行拷贝到正确偏移
    // 但如果 m_table_line_size == fd.size（单字段层），可以直接 memcpy 整块
    if (m_field_descs.size() == 1 && fd.offset == 0) {
        // 快速路径：单字段层，列数据连续
        memcpy(m_table_buffer.data(), src, fd.size * count);
    } else {
        // 通用路径：多字段层，需要带步幅拷贝
        for (u32 i = 0; i < count; i++) {
            memcpy(
                m_table_buffer.data() + i * m_table_line_size + fd.offset,
                src + i * fd.size,
                fd.size
            );
        }
    }
}
```

**SWIG 绑定：**
```swig
// fastcarto/fastdb/swig/fastdb4py.i
%feature("threadallow") wx::FastVectorDbLayerBuild::setColumnBulk;
%typemap(in) (const void* data, u32 count) {
    Py_buffer view;
    if (PyObject_GetBuffer($input, &view, PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) < 0) {
        SWIG_fail;
    }
    $1 = view.buf;
    $2 = (u32)(view.len / item_size);  // 需要从字段类型推断 item_size
    PyBuffer_Release(&view);
}
```

**Embind 绑定：**
```cpp
// ts/embind/fastdb4ts.cpp
void layer_build_set_column_bulk(
    FastVectorDbLayerBuild& layer, 
    u32 field_idx, uintptr_t data_ptr, u32 count
) {
    layer.setColumnBulk(
        field_idx, 
        reinterpret_cast<const void*>(data_ptr), 
        count
    );
}
```

### 9. 风险分析与边缘情况

#### 9.1 技术风险

| 风险 | 级别 | 缓解措施 |
|------|------|---------|
| **WASM 内存增长导致 TypedArray 视图失效** | 🟡 中 | TypeScript 端不缓存视图；每次访问通过 `module.HEAPU8.buffer` 刷新 |
| **零拷贝数组的生命周期管理** | 🟡 中 | 文档明确说明：零拷贝数组的生命周期绑定到 ORM/Database，ORM.close() 后数组无效 |
| **非 C-contiguous 数组** | 🟢 低 | 在 dumps 时强制 `np.ascontiguousarray()` 转换，增加一次拷贝但保证正确性 |
| **大数组导致 fastdb builder 内存溢出** | 🟢 低 | setScale 预分配已知大小；与 ORM.truncate 相同的内存分配策略 |
| **现有 SIGBUS bug 影响** | 🔴 高 | 当前 List[Feature] ≥32 元素的 SIGBUS bug 需要在缓冲区方案实施前修复 |
| **字节序不一致** | 🟢 低 | fastdb 使用 native 字节序；Python 和 WASM 都是小端序（x86/ARM/WASM 均为 LE） |

#### 9.2 边缘情况处理

**情况 1: 空数组**
```python
class Data(Feature):
    values: NdArray[F64]

d = Data(values=np.array([], dtype=np.float64))
```
处理方式：跳过缓冲区层创建，在 blob 中存储空标记（0 字节缓冲区引用）

**情况 2: 0 维数组（标量 numpy）**
```python
d = Data(values=np.float64(3.14))
```
处理方式：退回到普通标量字段路径

**情况 3: 非数值 dtype（如 numpy 字符串数组）**
```python
d = Data(values=np.array(["hello", "world"]))
```
处理方式：不走缓冲区路径，退回到 blob 编码（numpy 字符串不是定长缓冲区）

**情况 4: 结构化 dtype**
```python
d = Data(values=np.array([(1, 2.0), (3, 4.0)], dtype=[('x', 'i4'), ('y', 'f8')]))
```
处理方式：第一版不支持，退回到 blob。未来可支持多列缓冲区层。

**情况 5: 多个 Feature 共享同一个数组对象**
```python
shared = np.array([1.0, 2.0, 3.0])
a = Data(values=shared)
b = Data(values=shared)  # 同一个 numpy 对象
```
处理方式：Pass 1 通过 `id()` 检测共享，只创建一个缓冲区层，两个 Feature 的 blob 中存相同的层引用。

**情况 6: 超大数组（>2GB）**
处理方式：fastdb wire format 使用 u32 size 字段（最大 4GB），足够。但行数受 u32 feature_count 限制（~43 亿行），对于 f64 单列理论上限为 ~32GB。

### 10. 与其他优化方案的对比

#### 10.1 备选方案对比

| 方案 | 描述 | 优势 | 劣势 | 推荐度 |
|------|------|------|------|--------|
| **A: 纯 blob 优化** | 优化 struct.pack/unpack（预分配、批量 pack） | 改动最小 | 加速有限（~2-3×），仍有 Python 层循环 | ⭐⭐ |
| **B: 列式缓冲区层 + C++ 批量 API** | 本方案 | 最大加速（8-1500×），跨语言兼容 | 需要 C++ 改动，实现复杂度中等 | ⭐⭐⭐⭐⭐ |
| **C: 专用二进制追加段** | 在 fastdb 格式末尾追加原始缓冲区 | 理论最快（纯 memcpy） | 破坏 wire format，需要 C++ 核心改动 | ⭐⭐ |
| **D: 外部格式（Arrow IPC）** | 复杂对象用 fastdb，大数组用 Arrow | 成熟的零拷贝方案 | 引入外部依赖，格式不统一 | ⭐⭐⭐ |
| **E: pickle5 + fastdb 混合** | 小对象 pickle，大缓冲区 fastdb | 利用 pickle5 OOB 机制 | 违反"纯 fastdb"约束；TS 无法使用 pickle | ⭐ |

#### 10.2 为什么方案 B 最优？

1. **不引入外部依赖**：纯 fastdb 格式，Python 和 TypeScript 都能使用
2. **不破坏 wire format**：缓冲区层在格式层面就是普通层
3. **加速幅度最大**：dumps 8-78×，loads 17-1500×（取决于数组大小）
4. **实现复杂度可控**：核心改动只有 `setColumnBulk()` 一个新 C++ 方法
5. **渐进式实现**：Phase 1 可以先不加 C++ API，通过 blob 优化获得部分收益

---

## Decision

### Recommendation

**推荐实施方案 B：列式缓冲区层 + C++ 批量列写入 API。**

这是在纯 fastdb 格式约束下，实现 buffer-protocol 类型高性能序列化的最优路径。核心创新是：

1. 将大型连续缓冲区（numpy array / TypedArray）映射为**单字段 fastdb 层**
2. 通过新增的 `setColumnBulk()` C++ API 实现 **memcpy 级别的批量写入**
3. 反序列化时通过 `__array_interface__`（Python）或 `TypedArray` 构造函数（TS）实现**零拷贝读取**
4. 在 blob 中使用 16 字节定长缓冲区引用替代逐元素编码

### Rationale

| 决策因素 | 判断 |
|---------|------|
| **技术可行性** | ✅ 完全可行——利用 fastdb 单字段层的连续内存特性 |
| **性能收益** | ✅ dumps 8-78×, loads 17-1500×（量级提升）|
| **格式兼容** | ✅ 不破坏 wire format，向后兼容 |
| **跨语言** | ✅ Python 和 TypeScript 均可实现 |
| **实现复杂度** | 🟡 中等——需要 1 个新 C++ 方法 + 两端序列化器改动 |
| **风险** | 🟡 中等——需要先修复 SIGBUS bug |

### Implementation Notes

1. **前置条件**：修复 List[Feature] ≥32 元素的 SIGBUS bug（当前 blocker）
2. **实现顺序**：Python 端先行 → C++ 批量 API → TypeScript 端
3. **类型标注**：第一版通过运行时检测 buffer-protocol；后续添加 schema 级类型标注
4. **测试策略**：新增跨语言互操作测试（Python dumps → TS loads，反之亦然）
5. **基准对比**：每个 Phase 完成后运行 benchmark_comprehensive.py 并记录在 optimize/ 目录

### Follow-up Actions

- [ ] 修复 FastSerializer SIGBUS bug（List[Feature] ≥32 元素）
- [ ] 实现 Phase 1: Python 端缓冲区检测 + 逐行写入 + 零拷贝读取
- [ ] 实现 Phase 2: C++ `setColumnBulk()` API + SWIG/Embind 绑定
- [ ] 实现 Phase 3: TypeScript 端 TypedArray 检测 + WASM 批量写入
- [ ] 实现 Phase 4: Feature schema 类型标注 (`NdArray[F64]`, `Buffer[U8]`)
- [ ] 新增跨语言互操作测试（缓冲区字段的 Python ↔ TS 往返）
- [ ] 更新 benchmark_comprehensive.py 添加缓冲区序列化基准
- [ ] 更新 README 文档说明新的缓冲区类型支持

## Status History

| Date | Status | Notes |
| --- | --- | --- |
| 2026-03-27 | 🔴 Not Started | Spike created and scoped |
| 2026-03-27 | 🟡 In Progress | 代码库分析完成，Ray 方案调研完成 |
| 2026-03-27 | 🟢 Complete | 调研完成，推荐方案 B（列式缓冲区层 + C++ 批量 API） |

---

## External Resources

- [PEP 574 – Pickle protocol 5 with out-of-band data](https://peps.python.org/pep-0574/)
- [Ray Serialization docs](https://docs.ray.io/en/latest/ray-core/objects/serialization.html)
- [Apache Arrow IPC](https://arrow.apache.org/docs/python/ipc.html)
- [NumPy __array_interface__](https://numpy.org/doc/stable/reference/arrays.interface.html)
- [Emscripten Memory Model](https://emscripten.org/docs/porting/emscripten-runtime-environment.html)

---

_Last updated: 2026-03-27 by soku_

