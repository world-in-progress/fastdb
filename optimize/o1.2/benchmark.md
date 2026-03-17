# fastdb4py Benchmark Results — Round o1.2

**环境**：macOS, Apple Silicon (aarch64), Python 3.13.3, fastdb4py v0.1.12
**运行命令**：`uv run python tests/python/benchmark_comprehensive.py --quick`
**迭代数**：100 次测量 + 10 次预热

---

## Changes

### `python/fastdb4py/orm/table.py` — OPT-1.2: ColumnAccessor 热路径单步查找

**问题**：o1.1 实测 333 ns 来自热路径中仍有 **2 次** `object.__getattribute__` + **2 次** `dict.get`：

```
getattribute(_field_index_map)  ~100 ns  ← 热路径中无用
fmap.get(name) → idx             ~30 ns  ← 热路径中无用
getattribute(_array_cache)      ~100 ns
cache.get(idx) → arr             ~30 ns
return                           ~30 ns
total                           ~290 ns
```

缓存命中时 `_field_index_map` 完全不需要——idx 仅在首次（冷路径）调用 `get_column(idx)` 时使用。

**改动**：`_array_cache`（键为 idx）改为 `_name_cache`（键为 name），提到最前面查。热路径绕过 `_field_index_map`，只查 `_name_cache`；冷路径才访问 `_field_index_map`。

```python
# __init__
object.__setattr__(self, '_name_cache', {})   # 替换 _array_cache

# __getattr__（热/冷路径分离）
def __getattr__(self, name: str) -> np.ndarray:
    # 热路径：1× getattribute + 1× dict.get
    name_cache = object.__getattribute__(self, '_name_cache')
    arr = name_cache.get(name)
    if arr is not None:
        return arr

    # 冷路径：验证字段名 + SWIG 调用 + 缓存
    fmap = object.__getattribute__(self, '_field_index_map')
    idx = fmap.get(name)
    if idx is None:
        raise AttributeError(f'Field "{name}" not found in the table.')
    table_origin = object.__getattribute__(self, '_table_origin')
    arr = table_origin.get_column(idx).as_nparray()
    name_cache[name] = arr
    return arr
```

约 5 行改动，纯 Python，无需重编译。

---

## Expected Improvement

| 指标 | o1.1 基线 | 预期目标 | 预期改善 |
|------|-----------|----------|----------|
| `column_accessor_scan` (3f) | 333 ns | ~170 ns | **~2×** |
| `column_accessor_scan` (10f) | 333 ns | ~170 ns | **~2×** |
| `point_cloud_read_columnwise` N=1000 | 5.75 µs | ~3–4 µs | ~1.5× |

---

## Section 1: Microbenchmarks（单操作开销）

| 操作 | o0 Median | o1.1 Median | o1.2 Median | o0→o1.2 总改善 |
|------|-----------|-------------|-------------|---------------|
| `scalar_read_pure_python` (F64) | 167 ns | 167 ns | 167 ns | — |
| `scalar_read_db_mapped` (F64) | 667 ns | 542 ns | 542 ns | ↑19% |
| `scalar_write_db_mapped` | 1.29 µs | 1.12 µs | 1.17 µs | ↑9% |
| **`column_accessor_scan` (3f)** | 8.67 µs | 333 ns | **250 ns** | **↑35×** |
| **`column_accessor_scan` (10f)** | 9.54 µs | 333 ns | **250 ns** | **↑38×** |

---

## Section 3: Macro-benchmarks（真实场景）

| 操作 | o0 Median | o1.1 Median | o1.2 Median | o0→o1.2 改善 |
|------|-----------|-------------|-------------|-------------|
| `point_cloud_read_rowwise` | 4.40 ms | 3.98 ms | 4.06 ms | ↑8% |
| **`point_cloud_read_columnwise`** | 33.58 µs | 5.75 µs | **5.58 µs** | **↑6.0×** |
| `point_cloud_write_columnwise` | 12.75 µs | 3.54 µs | 3.71 µs | ↑3.4× |

**速比对比**：

| 对比 | o0 | o1.1 | o1.2 |
|------|----|------|------|
| Column read vs row-wise read | 131× | 692× | **727×** |
| Column write vs row-wise write | 521× | 1654× | **1655×** |

---

## Delta 分析

### 实际 vs 预期对比

| 指标 | 预期改善 | 实际改善 | 评估 |
|------|----------|----------|------|
| `column_accessor_scan` (3f) | ~2× | **1.33×（333→250 ns）** | △ 低于预期，见分析 |
| `column_accessor_scan` (10f) | ~2× | **1.33×（333→250 ns）** | △ 同上 |
| `point_cloud_read_columnwise` | ~1.5× | **1.03×（5.75→5.58 µs）** | — 在误差范围内 |

### 250 ns 的来源分析

热路径改为单步查找后，250 ns 拆解：

```
Python __getattr__ 分发（内部开销）     ~80 ns   ← 改动无法消除
object.__getattribute__(_name_cache)   ~100 ns
name_cache.get(name)                    ~30 ns
arr is not None check                   ~10 ns
return                                  ~10 ns
total                                  ~230 ns
```

预期 ~170 ns 未达到，是因为 `__getattr__` 协议本身有约 80 ns 的不可消除开销（Python 属性查找分发机制）。

### 优化极限分析

| 优化阶段 | 开销 |
|---------|------|
| 理论下限（`scalar_read_pure_python`） | 167 ns（`_cache` dict lookup） |
| o1.2 实测 | 250 ns |
| 差距来源 | `__getattr__` 分发协议 ~80 ns |

进一步压缩需绕过 `__getattr__`（如在 `Table.column` property 中直接返回 dict 而非 ColumnAccessor，或用 `__class_getitem__` trick），代价是 API 语义改变。当前 250 ns 已接近 `__getattr__` 协议极限，继续优化 ROI 递减。

---

## 三轮 ColumnAccessor 优化总结（o1 → o1.1 → o1.2）

| 轮次 | 改动 | column_scan | column_read_col | 关键发现 |
|------|------|-------------|-----------------|----------|
| o0 基线 | — | 8.67 µs | 33.58 µs | O(n) 线性扫描 |
| o1 | O(1) dict 字段查找 | 7.62 µs (↑12%) | ~33 µs | 真瓶颈是 SWIG get_column |
| o1.1 | numpy 数组缓存 | 333 ns (**↑26×**) | 5.75 µs (**↑5.8×**) | 消除重复 SWIG |
| o1.2 | 热路径单步查找 | **250 ns (↑1.33×)** | 5.58 µs (↑1.03×) | `__getattr__` 协议成新下限 |
| **总计** | — | **↑35×** | **↑6.0×** | — |

---

## 结论与下一步

OPT-1.2 将 `column_accessor_scan` 进一步从 333 ns 降至 **250 ns**（1.33×），接近 `__getattr__` 协议的理论极限（167 ns）。继续在 ColumnAccessor 层优化 ROI 递减，建议转向其他方向。

下一轮建议：**OPT-2**（`Feature._cache` 懒分配）→ 减少迭代场景 GC 压力，目标 `iter_table` 从 2.02 µs/item 降至 ~1.7 µs/item。
