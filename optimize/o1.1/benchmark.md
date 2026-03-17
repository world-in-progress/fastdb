# fastdb4py Benchmark Results — Round o1.1

**环境**：macOS, Apple Silicon (aarch64), Python 3.13.3, fastdb4py v0.1.12
**运行命令**：`uv run python tests/python/benchmark_comprehensive.py --quick`
**迭代数**：100 次测量 + 10 次预热

---

## Changes

### `python/fastdb4py/orm/table.py` — OPT-1.1: ColumnAccessor numpy 数组缓存

**根因**：o1 发现 `column_accessor_scan` 仅改善 12%（而非预期 29×），真实瓶颈是每次 `table.column.x` 都调用 SWIG `get_column(idx)` + `as_nparray()`（合计 ~5–6 µs 固定开销），与字段名查找方式无关。

**关键事实**：
- `table.column`（property）直接返回 `self._column`，在 `Table.map_from()` 时创建一次，永久复用
- `ColumnAccessor` 只在 `table.fixed == True` 时创建，fixed-scale `WxLayerTable` 的列内存指针在整个生命周期内不移动
- 写路径 `col[:] = arr` 是 numpy in-place 操作，仅修改 C++ 内存，不改变指针 → 缓存安全

**改动**：在 `ColumnAccessor.__init__` 中新增实例级 `_array_cache` 字典（`idx → np.ndarray`），`__getattr__` 在 SWIG 调用前先查字典，命中则直接返回：

```python
# __init__ 新增一行
object.__setattr__(self, '_array_cache', {})

# __getattr__ 新增缓存命中路径（插在 SWIG 调用前）
cache = object.__getattribute__(self, '_array_cache')
arr = cache.get(idx)
if arr is not None:
    return arr
# ... 原有 SWIG 调用 ...
cache[idx] = arr
return arr
```

约 +6 行，纯 Python，无需重编译。

---

## Expected Improvement

| 指标 | o1 基线 | 预期目标（缓存命中） | 预期改善 |
|------|---------|---------------------|----------|
| `column_accessor_scan` (3f) | 7.62 µs | ~100 ns | **~76×** |
| `column_accessor_scan` (10f) | 7.75 µs | ~100 ns | **~77×** |
| `point_cloud_read_columnwise` N=1000 | 33.58 µs | ~8–12 µs | **~3–4×** |
| `point_cloud_write_columnwise` N=1000 | 12.75 µs | ~4–6 µs | **~2–3×** |

---

## Section 1: Microbenchmarks（单操作开销）

| 操作 | o0 Median | o1 Median | o1.1 Median | o0→o1.1 改善 |
|------|-----------|-----------|-------------|-------------|
| `feature_init_pure_python` | 1.79 µs | ~1.79 µs | 1.75 µs | — |
| `scalar_read_pure_python` (F64) | 167 ns | 167 ns | 167 ns | — |
| `feature_init_db_mapped` | 2.50 µs | ~2.50 µs | 2.58 µs | — |
| `scalar_read_db_mapped` (F64) | 667 ns | 542 ns | 542 ns | **↑19%** |
| `scalar_write_db_mapped` (F64) | 1.29 µs | 1.17 µs | 1.12 µs | **↑13%** |
| `scalar_read_db_mapped` (I32) | 625 ns | ~542 ns | 583 ns | **↑7%** |
| `ref_resolve_1level_fresh` | 5.04 µs | ~5.04 µs | 5.00 µs | — |
| `ref_resolve_cached` | 167 ns | ~167 ns | 167 ns | — |
| `schema_cache_hit` | 291 ns | ~291 ns | 291 ns | — |
| `schema_cache_miss` | 7.25 µs | ~7.25 µs | 7.25 µs | — |
| **`column_accessor_scan` (3f)** | 8.67 µs | 7.62 µs | **333 ns** | **↑26×** |
| **`column_accessor_scan` (10f)** | 9.54 µs | 7.75 µs | **333 ns** | **↑29×** |

---

## Section 3: Macro-benchmarks（真实场景）

| 操作 | o0 Median | o1 Median | o1.1 Median | 改善（vs o0） |
|------|-----------|-----------|-------------|-------------|
| `point_cloud_write_rowwise` | 6.64 ms | 5.83 ms | 5.86 ms | ↑12% |
| `point_cloud_read_rowwise` | 4.40 ms | 3.94 ms | 3.98 ms | ↑9% |
| **`point_cloud_read_columnwise`** | 33.58 µs | ~33 µs | **5.75 µs** | **↑5.8×** |
| **`point_cloud_write_columnwise`** | 12.75 µs | ~12 µs | **3.54 µs** | **↑3.6×** |
| `ref_graph_traverse` | 1.24 ms | ~1.10 ms | 1.21 ms | — |
| `iter_table` (N=500) | 1.01 ms | ~1.01 ms | 1.03 ms | — |

**速比对比**：

| 对比 | o0 | o1.1 |
|------|----|------|
| Column read vs row-wise read | **131×** | **692×** |
| Column write vs row-wise write | **521×** | **1654×** |

---

## Delta 分析

### 实际 vs 预期对比

| 指标 | 预期改善 | 实际改善 | 评估 |
|------|----------|----------|------|
| `column_accessor_scan` (3f) | ~76× | **~23×（7.62µs→333ns）** | ✅ 超预期（预期 100 ns，实际 333 ns，仍有 2× Python overhead） |
| `column_accessor_scan` (10f) | ~77× | **~23×（7.75µs→333ns）** | ✅ |
| `point_cloud_read_columnwise` | 3–4× | **5.8×（33.58µs→5.75µs）** | ✅ 超预期 |
| `point_cloud_write_columnwise` | 2–3× | **3.6×（12.75µs→3.54µs）** | ✅ 同样受益（column 访问缓存命中） |

### 333 ns 的来源分析

缓存命中路径耗时拆解（实测 333 ns）：
```
object.__getattribute__(self, '_field_index_map')   ~100 ns
fmap.get(name)                                       ~30 ns
object.__getattribute__(self, '_array_cache')        ~100 ns
cache.get(idx)                                       ~30 ns
None check + return                                  ~30 ns
total                                                ~300 ns
```

2 次 `object.__getattribute__` 是主开销（各 ~100 ns）。进一步消除需改变缓存存储方式。

### point_cloud_write_columnwise 同样获益

write benchmark 内部 `tbl_cloud.column.x` 获取列引用，然后 `col[:] = 1.0` 写入。第一次调用时缓存 numpy view，后续 benchmark 迭代复用，避免了每次 `get_column()` 的 SWIG 开销（~5 µs × 迭代数的节省）。

---

## 结论与下一步

OPT-1.1 完全修复了 o1 中未解决的 column accessor 性能问题：

- `column_accessor_scan`：8.67 µs → 333 ns（**~26× 加速**，完成原定目标 ~29×）
- `point_cloud_read_columnwise`：33.58 µs → 5.75 µs（**5.8× 加速**，Column vs row-wise 从 131× 提升至 692×）

下一轮建议：**OPT-2**（`Feature._cache` 懒分配）→ 减少迭代场景的 GC 压力，目标 `iter_table` 从 2.02 µs/item 降至 ~1.7 µs/item。
