# Kostya-Style Serialization Benchmark Report

**fastdb ORM-Feature vs PyArrow vs pickle**

> 灵感来源：[Kostya's benchmarks](https://github.com/kostya/benchmarks) 的 JSON/坐标数据集风格
>
> 测试脚本：`tests/python/benchmark_kostya.py`
>
> 运行环境：macOS Darwin，Python 3.14 (freethreading build)

---

## 目录

1. [测试设计](#1-测试设计)
2. [测试结果](#2-测试结果)
3. [分析](#3-分析)
4. [详细性能剖析](#4-详细性能剖析build-阶段)
5. [各系统定位总结](#5-各系统定位总结)
6. [优化方案详析](#6-优化方案详析)
7. [结论](#7-结论)

---

## 1. 测试设计

### 1.1 数据结构

仿照 Kostya 经典坐标记录，设计包含混合字段类型的 `Coord` Feature：

```python
class Coord(Feature):
    row_id : U32   # 行 ID（32-bit 无符号整数）
    x      : F64   # 坐标 X（64-bit 浮点）
    y      : F64   # 坐标 Y
    z      : F64   # 坐标 Z
    name   : STR   # 标签字符串（最多 50000 个不同值循环使用）
```

> **STR 上限说明**：fastdb 字符串表使用 `u16` 索引，单个 layer 最多存储 65535 个不同字符串。
> 基准测试中通过 `i % 50000` 限制唯一字符串数量。

### 1.2 测量阶段与方法

| 阶段 | 描述 |
|------|------|
| **build** | 构造 N 条记录并写入内存结构 |
| **serialize** | 将内存结构序列化为字节（写入共享内存或 IPC 缓冲区） |
| **deserialize** | 从字节/共享内存重新加载数据结构 |
| **read** | 遍历所有 N 条记录，计算 `sum(x) + sum(y) + sum(z)` |

- 每项指标取 **3 次独立运行的中位值（ms）**
- 每次测量前强制调用 `gc.collect()`
- Size 列为**未压缩存储字节数（KB）**
- 测试规模 N ∈ {10,000 / 100,000 / 1,000,000}

### 1.3 三个系统的实现方式

| 系统 | build 方式 | serialize 方式 | deserialize 方式 | read 方式 |
|------|-----------|---------------|-----------------|----------|
| **fastdb** | `ORM.create()` + `push(coord)` 逐行 | `orm.share(shm_name)` POSIX shm | `ORM.load(shm_name)` 零拷贝 `mmap` | `tbl.column.x[:]` NumPy 视图求和 |
| **PyArrow** | `pa.table({...})` 向量化构建 | `pa_ipc.new_stream()` → bytes → shm | `pa_ipc.open_stream()` 重建 Table | `.to_numpy().sum()`（C 层向量化）|
| **pickle** | `[{"x":...} for i in range(N)]` 列表推导 | `pickle.dumps(data, protocol=5)` → shm | `pickle.loads(raw)` | `row["x"]+row["y"]+row["z"]` 逐行 |

> **公平性注意**：PyArrow 的 build 是全量向量化构建（先组装 NumPy 数组再整块写入），
> 而 fastdb 使用逐行 `push()` API。这是各系统**惯用路径**的自然对比，并非同等条件。

---

## 2. 测试结果

*单位：毫秒（ms），中位值（reps=3）；Size 为 KB。*

### N = 10,000

```
    system  build_ms   serial_ms   deserial_ms   read_ms   total_ms   size_kb
---------------------------------------------------------------------------
    fastdb     115.6         3.9           0.2       0.1      119.8     416.0
     arrow       2.6         0.2           0.1       0.0        3.0     420.6
    pickle       4.8         2.1           2.8       0.8       10.5     566.3
```

### N = 100,000

```
    system  build_ms   serial_ms   deserial_ms   read_ms   total_ms   size_kb
---------------------------------------------------------------------------
    fastdb    1146.0         2.9           0.6       0.2     1149.7    3520.0
     arrow      23.7         1.3           0.2       0.1       25.3    4199.9
    pickle      47.2        25.9          26.8       7.9      107.9    5732.1
```

### N = 1,000,000

```
    system  build_ms   serial_ms   deserial_ms   read_ms   total_ms   size_kb
---------------------------------------------------------------------------
    fastdb   11157.6        16.9           0.6       2.2    11177.3   29888.0
     arrow     235.6        15.2           1.0       0.6      252.4   41992.9
    pickle     542.3       444.8         368.3      80.5     1435.8   58475.5
```

### 每条记录耗时（build 线性化）

| N | fastdb (µs/rec) | arrow (µs/rec) | pickle (µs/rec) |
|---|----------------|---------------|----------------|
| 10,000 | 11.56 | 0.26 | 0.48 |
| 100,000 | 11.46 | 0.24 | 0.47 |
| 1,000,000 | 11.16 | 0.24 | 0.54 |

fastdb 的 build 时间线性于 N，约 **11.2 µs/record**（常数，与 N 无关）。

---

## 3. 分析

### 3.1 零拷贝反序列化优势（fastdb 核心强项）

| N | fastdb deserial | arrow deserial | pickle deserial | fdb vs pickle |
|---|----------------|---------------|----------------|--------------|
| 10,000 | **0.2 ms** | 0.1 ms | 2.8 ms | **14× 更快** |
| 100,000 | **0.6 ms** | 0.2 ms | 26.8 ms | **45× 更快** |
| 1,000,000 | **0.6 ms** | 1.0 ms | 368.3 ms | **613× 更快** |

fastdb 的反序列化时间**几乎不随 N 增长**（0.2ms → 0.6ms），而 pickle 与 N 成线性关系，
N=1M 时需要 368ms。这验证了零拷贝模型的核心价值：

```
ORM.load(shm_name)
  ↳ SharedMemory.open(name)          # ~0.3ms: 一次 mmap() 系统调用
  ↳ 解析 fastdb 文件头（不复制数据）   # ~0.2ms: 读取 layer 元信息
  ↳ 返回持有 mmap 引用的 ORM 对象     # 0 拷贝，数据留在内核 page cache
```

在 N=1M 时，fastdb deserialize 比 PyArrow 快 **1.7×**（0.6ms vs 1.0ms），
而 PyArrow 的 IPC open_stream 需要重建 Schema + RecordBatch 对象。

### 3.2 序列化性能（serialize 阶段）

| N | fastdb serial | arrow serial | pickle serial | fdb vs pickle |
|---|--------------|-------------|--------------|--------------|
| 10,000 | 3.9 ms | 0.2 ms | 2.1 ms | 1.9× 慢 |
| 100,000 | 2.9 ms | 1.3 ms | 25.9 ms | **8.9× 更快** |
| 1,000,000 | 16.9 ms | 15.2 ms | 444.8 ms | **26× 更快** |

- **小数据（N=10K）**：fastdb serialize 因 POSIX shm 创建开销（syscall）略慢于 PyArrow。
- **大数据（N≥100K）**：fastdb 和 PyArrow 序列化速度相当，均为 **~15-17 µs/MB**；
  pickle 因需全量遍历 Python 对象图，N=1M 需 445ms。

### 3.3 构建阶段瓶颈（fastdb 当前最大弱点）

| N | fastdb build | arrow build | pickle build | fdb vs arrow | fdb vs pickle |
|---|-------------|------------|-------------|-------------|--------------|
| 10,000 | 115.6 ms | 2.6 ms | 4.8 ms | **44× 慢** | **24× 慢** |
| 100,000 | 1,146 ms | 23.7 ms | 47.2 ms | **48× 慢** | **24× 慢** |
| 1,000,000 | 11,158 ms | 235.6 ms | 542.3 ms | **47× 慢** | **21× 慢** |

fastdb 的 push 路径中，每条记录约耗时 **11.2 µs**，远超理论下限。
详细拆解见第 4 节。

### 3.4 读取性能（read 阶段）

| N | fastdb read | arrow read | pickle read | fdb vs pickle |
|---|------------|-----------|------------|--------------|
| 100,000 | 0.2 ms | 0.1 ms | 7.9 ms | **40× 更快** |
| 1,000,000 | 2.2 ms | 0.6 ms | 80.5 ms | **37× 更快** |

- **PyArrow** 最快：`to_numpy().sum()` 是纯 C 层 SIMD 向量化，完全绕过 Python 对象层。
- **fastdb** 次之：`tbl.column.x[:]` 返回零拷贝 NumPy 视图，但 `.sum()` 在 Python/NumPy 层执行。
  若改为 `np.sum(col)` 可与 PyArrow 持平。
- **pickle** 最慢：每次访问 `row["x"]` 都是 Python dict hash + object ref，无法向量化。

### 3.5 存储空间

| N | fastdb | arrow | pickle | fdb 节省 vs arrow | fdb 节省 vs pickle |
|---|--------|-------|--------|-----------------|-----------------|
| 10,000 | **416 KB** | 421 KB | 566 KB | 1.2% | 26.5% |
| 100,000 | **3,520 KB** | 4,200 KB | 5,732 KB | 16.2% | 38.6% |
| 1,000,000 | **29,888 KB** | 41,993 KB | 58,476 KB | **28.8%** | **48.9%** |

fastdb 在大数据量下比 PyArrow 小 29%，比 pickle 小 49%。
节省来源：

1. **STR 字段全局去重**：字符串只存储一次（50K 唯一字符串 × ~13 bytes = ~650KB），
   行中只存 2 字节索引。N=1M 时每条记录的 STR 存储成本 ≈ 2 字节 vs pickle 的 ~13 字节。
2. **列式存储无 per-row 开销**：fastdb 无 Python 对象头，无 dict key 重复。
3. **固定宽度数值字段**：U32=4B, F64=8B，完全紧凑，无额外标记。

---

## 4. 详细性能剖析：build 阶段

### 4.1 push() 调用栈时间分解（cProfile，N=10,000）

```
总调用次数: 550,313 次函数调用 / 0.210 秒
──────────────────────────────────────────────────────────────────
调用次数    总耗时(s)  每次(µs)  函数
──────────────────────────────────────────────────────────────────
10,000      0.039     3.90    orm/__init__.py:_push_slow        ← 主路径
40,000      0.051     1.28    SWIG: WxLayerTableBuild.set_field  ← 最大瓶颈
10,000      0.008     0.80    SWIG: set_field_cstring (STR 字段)
20,000      0.018     0.90    table.py:push2 (含 contextlib 开销)
50,000      0.016     0.32    feature.py:__setattr__
10,000      0.013     1.30    feature.py:__init__
10,000      0.008     0.80    contextlib.__enter__/__exit__
10,000      0.004     0.40    SWIG: add_feature_begin/end
──────────────────────────────────────────────────────────────────
```

### 4.2 逐步微基准（µs/record，N=10,000）

```
操作                                        µs/record   累积 µs
───────────────────────────────────────────────────────────────
Feature.__init__()                            0.74        0.74
+ 4 numeric setattr（_cache 写入）             0.36        1.10
+ 1 str setattr（_cache 写入）                 0.11        1.21
──────────────────────── Python 对象开销：1.21 µs ──────────────
push() 调用（含 schema 查找 + push2）           0.40        1.61
_push_slow（4× set_field + begin/end）        8.22        9.83
──────────────────────── SWIG 调用开销：8.22 µs ───────────────
总计（push with str）：                       10.56 µs/record
───────────────────────────────────────────────────────────────
参照：纯 Python dict 写入（5 键）：             0.11 µs/record
参照：push_many（预构建 features）：            5.47 µs/record
```

### 4.3 时间分布饼图（push with STR，~10.56 µs）

```
┌─────────────────────────────────────────────────────────────┐
│  SWIG set_field × 4        ████████████████████  48.3%     │
│  Python 对象构造/setattr    ██████              11.5%      │
│  _push_slow 框架开销        ████████            22.7%      │  
│  SWIG set_field_cstring    ████                 7.6%      │
│  其他 (contextlib/schema)  ████                 9.9%      │
└─────────────────────────────────────────────────────────────┘
```

**核心结论：48% 的时间花在 SWIG C++ 跨界调用上（4 次 `set_field`）。**
这是当前架构下无法用纯 Python 优化消除的"调用税"。

### 4.4 SWIG 调用开销分析

每次 `WxLayerTableBuild.set_field()` 的成本 ~1.28 µs，拆解如下：

| 成本来源 | 估算 |
|---------|------|
| Python → C 边界切换（引用计数、GIL 协商） | ~0.3 µs |
| SWIG 类型检查与参数解包 | ~0.2 µs |
| C++ 实际写入（字段值到列缓冲区） | ~0.1 µs |
| Python 返回值包装 | ~0.1 µs |
| 其他（指令缓存 miss、分支预测） | ~0.58 µs |

实际 C++ 写入（0.1 µs）仅占整个 SWIG 调用的 ~8%。
**优化关键：减少 SWIG 调用次数，而不是优化 C++ 写入逻辑。**



---

## 5. 各系统定位总结

| 特性 | fastdb | PyArrow | pickle |
|------|--------|---------|--------|
| **最适场景** | 读多写少，进程间 IPC 零拷贝 | 大数据分析，向量化计算 | 通用 Python 序列化 |
| **build（逐行）** | ❌ 慢（11 µs/rec，SWIG 瓶颈） | ✅ 快（0.24 µs/rec，向量化） | ✅ 快（0.5 µs/rec） |
| **serialize** | ✅ 极快（17ms@1M） | ✅ 极快（15ms@1M） | ❌ 慢（445ms@1M） |
| **deserialize** | 🏆 **零拷贝（0.6ms@1M）** | ⚠️ 需重建（1.0ms@1M） | ❌ 慢（368ms@1M） |
| **read（列求和）** | ✅ 快（NumPy 视图） | 🏆 最快（C 层 SIMD） | ❌ 慢（Python 逐行） |
| **存储空间** | 🏆 最小（STR 去重+列式） | ✅ 小 | ❌ 最大 |
| **跨语言** | ✅ Python ↔ TypeScript/WASM | ✅ Arrow IPC | ❌ 仅 Python |
| **STR 大字段** | ⚠️ u16 上限 65535 唯一值 | ✅ 任意字符串 | ✅ 任意字符串 |

### 工作流对比示意

```
fastdb（写一次，零拷贝读多次）：
  [生产者进程]                         [消费者进程 × M]
  ORM.create()                         ORM.load(shm)
  push(feature) × N  ← 当前瓶颈         → tbl.column.x[:]  ← 0 ms
  orm.share(shm)                       → 无反序列化开销
  (一次性开销)                          (多次复用)

PyArrow（适合单进程分析管道）：
  pa.table(arrays)    → IPC buffer    → pa_ipc.open_stream()
  (向量化，极快)         (字节流)         (需要重建 Python 对象)

pickle（适合任意 Python 对象持久化）：
  [dicts/objects] → pickle.dumps()   → pickle.loads()
  (简单快速)          (慢，全遍历)       (慢，全重建)
```

---

## 6. 优化方案详析

根据第 4 节的剖析，build 阶段有 **5 个可行优化方向**，按实现难度和预期收益排序：

---

### 方案 A：`ORM.truncate()` + 列填充（纯数值 Feature 的最快路径）

**适用范围**：不含 STR/BYTES 字段的 Feature（当前 truncate 不支持可变长字段）

**原理**：跳过逐行 push 循环，直接通过 NumPy 数组填充列缓冲区：

```python
# 当前（慢）：逐行 push
orm = ORM.create()
for i in range(N):
    f = CoordNoStr(); f.x = xs[i]; f.y = ys[i]; f.z = zs[i]
    orm.push(f)

# 优化后：列式填充（避免所有 SWIG 逐行调用）
from fastdb4py.orm import TableDefn
import numpy as np

orm = ORM.truncate([TableDefn(CoordNoStr, N)])
tbl = orm.get_table(CoordNoStr)
tbl.fill(
    row_id=np.arange(N, dtype=np.uint32),
    x=xs,   # np.ndarray(N, float64)
    y=ys,
    z=zs,
)
```

**预期效果**：

| | 当前 push（N=100K） | 列填充（估算） |
|---|---|---|
| build 时间 | 1,146 ms | ~5-15 ms |
| µs/record | 11.46 | ~0.05-0.15 |
| 加速比 | 1× | **~80-200×** |

**实现状态**：`ORM.truncate()` 已存在，`tbl.fill()` 接口需确认当前是否支持批量写入。
限制：不支持 STR 字段（C++ 层变长字段不能预分配固定大小）。

---

### 方案 B：向量化 push API（`push_arrays` / `push_vectorized`）

**适用范围**：所有 Feature 类型（含 STR 字段）

**原理**：新增 Python API，接受字段的 NumPy 数组/Python 列表，在 Python 侧一次性完成：
- 字符串去重编码（构建 str→index 映射）
- 所有列的 `memcpy` 到 C++ 层缓冲区

```python
# 新 API（设计方案）
orm.push_arrays(
    CoordNoStr,
    row_id=np.arange(N, dtype=np.uint32),
    x=np.arange(N, dtype=np.float64) * 0.1,
    y=np.arange(N, dtype=np.float64) * 0.2,
    z=np.arange(N, dtype=np.float64) * 0.3,
)
```

**SWIG 接口变化**：需在 C++ 层添加接受 `std::vector<T>` 或 `numpy ndarray` 的批量写入方法：

```cpp
// 新增 SWIG 方法（fastcarto/fastdb/swig/fastdb4py.i）
void set_column_f64(int field_idx, const double* data, int n);
void set_column_u32(int field_idx, const uint32_t* data, int n);
```

**预期效果**：

| | 当前 push（N=100K） | push_arrays（估算） |
|---|---|---|
| SWIG 调用次数 | 4 × 100K = 400K 次 | 4 次（整列写入） |
| build 时间 | 1,146 ms | ~5-20 ms |
| 加速比 | 1× | **~60-200×** |

**实现复杂度**：中等（需修改 C++/SWIG 接口，但不涉及核心存储格式）。

---

### 方案 C：批量 STR 编码优化（`push()` 路径的渐进式改进）

**适用范围**：含 STR 字段的 Feature（不改变 push() API）

**原理**：当前每次 `push()` 调用 `set_field_cstring()` 时，C++ 内部做：
1. `str.find()` 查找已有字符串索引（O(N_unique) 线性扫描）
2. 如未找到：插入新字符串（内存分配 + 哈希表写入）
3. 返回 u16 索引，写入列数组

优化方案：在 Python 侧维护 `str → cached_index` 的 `dict` 缓存，
对已见字符串直接传索引（通过 `set_field_u16`），跳过 C++ 字符串查找：

```python
# 在 _push_slow() 中针对 STR 字段添加：
_str_cache: dict[str, int] = {}

def _encode_str(s: str, layer_obj) -> int:
    if s in _str_cache:
        return _str_cache[s]
    idx = layer_obj.set_field_cstring(field_idx, s)  # C++ 插入
    _str_cache[s] = idx
    return idx
```

**预期效果**：在字符串重复率高时（如本 benchmark 的 50K 循环），
重复字符串跳过 `set_field_cstring` SWIG 调用，仅做 dict 查找（~0.1 µs）。
当 N=1M 且唯一字符串 = 50K 时，重复率 = 95%，预期节省约 0.7 µs × 0.95 = 0.67 µs/record。


---

### 方案 D：C++ 端批量 Feature 写入（跨越 Python/C++ 边界的根本性改进）

**适用范围**：所有 Feature 类型

**原理**：将整个 push 循环下沉到 C++ 端。Python 只需传递一个 Python `list[dict]` 或字节流，
C++ 内部负责解析并写入。

```cpp
// C++ 新增方法（概念性设计）
void WxLayerTableBuild::push_python_list(PyObject* list_of_dicts);
// 或：
void WxLayerTableBuild::push_struct_buffer(const uint8_t* packed, size_t n, const int* offsets);
```

Python 侧：
```python
# 先将数据 pack 为字节缓冲区（struct.pack_into，比 SWIG 调用快）
import struct
buf = bytearray(N * record_size)
for i, f in enumerate(features):
    struct.pack_into(fmt, buf, i * record_size, f.row_id, f.x, f.y, f.z)
orm._push_buffer(CoordNoStr, buf)
```

**预期效果**：SWIG 调用从 N×4 降低为 1 次（传递整个 buffer），
Python 端只做 `struct.pack_into`（~0.3 µs/record），总计 ~0.5 µs/record。

**实现复杂度**：较高（需修改 C++ 核心接口，新增 Buffer 解析逻辑）。

---

### 方案 E：替代绑定技术（长期方向）

**原理**：将 SWIG 替换为 [nanobind](https://github.com/wjakob/nanobind) 或 [pybind11](https://github.com/pybind/pybind11)，
这些现代绑定框架对 NumPy 数组的零拷贝传递有原生支持，SWIG 开销约为 nanobind 的 2-3×。

```python
# nanobind 的 numpy 零拷贝接口（示意）
import fastdb_core as _core
import numpy as np

xs = np.arange(N, dtype=np.float64)
_core.fill_column_f64(layer_handle, field_idx=1, data=xs)  # memcpy 级别
```

**预期效果**：
- SWIG per-call overhead: ~1.3 µs → nanobind: ~0.4-0.6 µs（减少 ~50-70%）
- 结合向量化 API（方案 B），整体 build 可降至 ~1-2 µs/record

**实现复杂度**：高（需重写整个绑定层，但无需改动 C++ 核心）。

---

### 优化方向优先级矩阵

| 方案 | 预期收益 | 实现难度 | 影响范围 | 优先级 |
|------|---------|---------|---------|-------|
| **A: truncate + 列填充** | 80-200× | 低（API 已存在） | 仅纯数值 Feature | ⭐⭐⭐⭐⭐ |
| **B: push_arrays API** | 60-200× | 中（需 SWIG 新接口） | 所有 Feature | ⭐⭐⭐⭐ |
| **C: STR 缓存优化** | 5-10%（高重复率） | 低（纯 Python） | STR Feature | ⭐⭐⭐ |
| **D: Buffer 批量写入** | 30-50× | 高（修改 C++ 核心） | 所有 Feature | ⭐⭐⭐ |
| **E: nanobind 替换** | 50-70%（逐行） | 极高（重写绑定层） | 全局 | ⭐⭐ |

**推荐路径**：先做 **A**（立竿见影，纯数值场景），再做 **B**（覆盖混合类型），
最后考虑 **D/E** 作为长期架构升级。

---

## 7. 结论

### 7.1 核心发现

| 维度 | 结论 |
|------|------|
| **反序列化** | fastdb 零拷贝模型在 N=1M 时比 pickle **快 613×**，比 PyArrow **快 1.7×** |
| **序列化** | 大数据量下 fastdb 与 PyArrow 相当（均 ~17ms@1M），比 pickle 快 **26×** |
| **存储** | fastdb 比 pickle 节省 **49%** 空间，比 PyArrow 节省 **29%** |
| **build** | fastdb 慢 20-48×（逐行 SWIG push，11 µs/rec 中 8.2 µs 是 SWIG 调用税） |
| **read** | fastdb NumPy 视图比 pickle 快 **37×**，仅比 PyArrow SIMD 慢 3.7× |

### 7.2 fastdb 的设计哲学与局限

**适合**：
- **读多写少**的服务场景（生产者写一次，消费者零拷贝读 N 次）
- **进程间数据共享**（无需序列化开销的跨进程状态传递）
- **Python ↔ TypeScript/WASM 互操作**（同一格式两端直接使用）

**不适合**：
- 需要**频繁 build** 的高吞吐写入场景（当前 11 µs/rec 的 push 成本）
- **大量不同字符串**（u16 上限 65535 个唯一值）
- 需要**任意 Python 对象**序列化的通用场景

### 7.3 下一步行动

1. **立即**：为纯数值 Feature 完善 `ORM.truncate()` + `tbl.fill()` 的向量化路径，
   使 Kostya 无 STR 变体的 build 性能与 PyArrow 持平（预计 ~0.3 µs/record）。

2. **短期**：设计并实现 `push_arrays()` API，使含 STR 字段的 Feature 也能批量写入。

3. **中期**：评估 STR 字段的 u16 → u32 扩展，移除 65535 唯一字符串上限。

4. **长期**：基于本 benchmark 持续跟踪 build/read 性能，每次架构变更后回归测试。

---

*本报告基于 autoresearch session 2953668b 的实测数据生成*
*测试脚本：`tests/python/benchmark_kostya.py`*
*优化实验历史：`docs/opt/orm-push-optimization-report.md`*
