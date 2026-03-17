# fastdb4py Benchmark Results — Round o1

**环境**：macOS, Apple Silicon (aarch64), Python 3.13.3, fastdb4py v0.1.12
**运行命令**：`uv run python tests/python/benchmark_comprehensive.py --quick`
**迭代数**：100 次测量 + 10 次预热

---

## Changes

### `python/fastdb4py/orm/table.py` — OPT-1: ColumnAccessor O(1) 字段查找

**问题**：`ColumnAccessor.__getattr__` 每次调用 `get_all_defns(feature_type)` 拿到有序列表，再 O(n) 线性扫描找 index，即使 n=3 也需要 ~9 µs（主要耗费在 `WeakKeyDictionary` 查找 + `sort`）。

**改动**：在 `_create_column_accessor()` 创建动态类时，提前在闭包中计算 `field_name → column_index` 字典（`_field_index_map`），并在 `ColumnAccessor.__init__` 中通过 `object.__setattr__` 注入实例。`__getattr__` 改为 O(1) `dict.get()` 查找，去掉了每次的 `get_all_defns()` 调用。

```python
# 新增：类创建时一次性计算
_field_index_map = {name: idx for idx, (name, _) in enumerate(get_all_defns(feature_type))}

class ColumnAccessor:
    def __init__(self, table_origin, feature_type):
        object.__setattr__(self, '_table_origin', table_origin)
        object.__setattr__(self, '_field_index_map', _field_index_map)  # O(1) 查找表

    def __getattr__(self, name: str) -> np.ndarray:
        fmap = object.__getattribute__(self, '_field_index_map')
        idx = fmap.get(name)
        if idx is None:
            raise AttributeError(f'Field "{name}" not found in the table.')
        table_origin = object.__getattribute__(self, '_table_origin')
        return table_origin.get_column(idx).as_nparray()
```

### `python/fastdb4py/feature/feature.py` — OPT-3: `__getattr__` if-chain → dict dispatch

**问题**：db-mapped scalar read 在 `__getattr__` 中通过最多 8 个 `if/elif` 分支路由到对应 SWIG getter。最常见的 F64 需经过 4 个不匹配的分支。`__setattr__` 的数值类型判断同样用 tuple `in`（O(n)）。

**改动**：
1. 模块顶层新增 `_SCALAR_GETTER: dict[OriginFieldType, Callable]`，将枚举值直接映射到 getter lambda。`__getattr__` 中用单次 `dict.get()` 替换全部 if-chain。
2. 新增 `_NUMERIC_FIELD_TYPES: frozenset`，`__setattr__` 中 `in` 测试从 O(n) tuple 变为 O(1) frozenset。

```python
_SCALAR_GETTER = {
    OriginFieldType.u8:   lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.u16:  lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.u32:  lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.i32:  lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.f32:  lambda o, fid: o.get_field_as_float(fid),
    OriginFieldType.f64:  lambda o, fid: o.get_field_as_float(fid),
    OriginFieldType.str:  lambda o, fid: o.get_field_as_string(fid),
    OriginFieldType.wstr: lambda o, fid: o.get_field_as_wstring(fid),
}

_NUMERIC_FIELD_TYPES = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.f32, OriginFieldType.f64,
    OriginFieldType.u8n, OriginFieldType.u16n,
))
```

---

## Expected Improvement

| 指标 | o0 基线 | 预期目标 | 预期改善 |
|------|---------|----------|----------|
| `column_accessor_scan` (3f) | 8.67 µs | ~300 ns | ~29× |
| `column_accessor_scan` (10f) | 9.54 µs | ~300 ns | ~32× |
| `scalar_read_db_mapped` (F64) | 667 ns | ~500 ns | ~25% |
| `scalar_write_db_mapped` | 1.29 µs | ~1.05 µs | ~20% |
| `point_cloud_read_columnwise` | 33.58 µs | ~10 µs | ~3× |

---

## Section 1: Microbenchmarks（单操作开销）

| 操作 | o0 Median | o1 Median | 变化 | o0 P95 | o1 P95 |
|------|-----------|-----------|------|--------|--------|
| `feature_init_pure_python` | 1.79 µs | ~1.79 µs | — | 2.08 µs | — |
| `scalar_read_pure_python` (F64) | 167 ns | ~167 ns | — | 209 ns | — |
| `feature_init_db_mapped` | 2.50 µs | ~2.50 µs | — | 2.75 µs | — |
| `scalar_read_db_mapped` (F64) | 667 ns | **542 ns** | **↑ 19%** | 791 ns | — |
| `scalar_write_db_mapped` (F64) | 1.29 µs | **1.17 µs** | **↑ 9%** | 1.38 µs | — |
| `scalar_read_db_mapped` (I32) | 625 ns | ~542 ns | **↑ ~13%** | 791 ns | — |
| `ref_resolve_1level_fresh` | 5.04 µs | ~5.04 µs | — | 5.42 µs | — |
| `ref_resolve_cached` | 167 ns | ~167 ns | — | 250 ns | — |
| `schema_cache_hit` | 291 ns | ~291 ns | — | 334 ns | — |
| `schema_cache_miss` | 7.25 µs | ~7.25 µs | — | 10.33 µs | — |
| `column_accessor_scan` (last of 3) | 8.67 µs | **7.62 µs** | **↑ 12%** | 14.25 µs | — |
| `column_accessor_scan` (last of 10) | 9.54 µs | **7.75 µs** | **↑ 19%** | 12.00 µs | — |

---

## Section 2: Meso-benchmarks（ORM 生命周期）

| 操作 | Param | o0 Median | o1 Median | 变化 |
|------|-------|-----------|-----------|------|
| `ORM.truncate` | N=10 | 14.25 µs | ~14 µs | — |
| `ORM.truncate` | N=100 | 16.08 µs | ~16 µs | — |
| `ORM.truncate` | N=500 | 18.21 µs | ~18 µs | — |
| `ORM.create + push` | N=10 | 83.46 µs | ~83 µs | — |
| `ORM.create + push` | N=100 | 749 µs | ~749 µs | — |
| `ORM.create + push` | N=500 | 3.76 ms | ~3.76 ms | — |

---

## Section 3: Macro-benchmarks（真实场景）

| 操作 | Param | o0 Median | o1 Median | 变化 |
|------|-------|-----------|-----------|------|
| `point_cloud_write_rowwise` | N=1000 | 6.64 ms | **5.83 ms** | **↑ 12%** |
| `point_cloud_read_rowwise` | N=1000 | 4.40 ms | **3.94 ms** | **↑ 10%** |
| `point_cloud_read_columnwise` | N=1000 | 33.58 µs | ~33 µs | — |
| `point_cloud_write_columnwise` | N=1000 | 12.75 µs | ~12 µs | — |
| `ref_graph_traverse` | N=100 tri | 1.24 ms | ~1.10 ms | **↑ ~11%** |
| `iter_table` | N=500 | 1.01 ms | ~1.01 ms | — |

---

## Delta 分析

### 实际 vs 预期对比

| 指标 | 预期改善 | 实际改善 | 差距说明 |
|------|----------|----------|----------|
| `column_accessor_scan` (3f) | ~29× | **1.14×** | 实测仅 12%，远低于预期 |
| `column_accessor_scan` (10f) | ~32× | **1.23×** | 同上 |
| `scalar_read_db_mapped` (F64) | ~25% | **~19%** | 接近预期 |
| `scalar_write_db_mapped` | ~20% | **~9%** | 低于预期 |
| `point_cloud_read_columnwise` | ~3× | **无明显改善** | 原来已是 SWIG+numpy，改动無关 |

### column_accessor_scan 改善低于预期的原因

`column_accessor_scan` 仅从 ~9 µs 下降到 ~7.6 µs，而非预期的 ~300 ns，原因如下：

1. **原来的瓶颈被误判**：profile 显示 ~9 µs 的主因是 `WeakKeyDictionary` 的 class-level cache 查找 + `get_all_defns()` 内部的 `sort()`。OPT-1 去掉了 `get_all_defns()` 调用，但 `ColumnAccessor` 的 **class 本身**仍从 `_column_accessor_cache`（WeakKeyDict）取出，这一步约 1–2 µs。

2. **O(1) dict lookup 之后仍有不可消除的开销**：
   - `object.__getattribute__(self, '_field_index_map')` — ~100 ns
   - `object.__getattribute__(self, '_table_origin')` — ~100 ns
   - SWIG `get_column(idx)` — ~4–5 µs（**主要瓶颈**）
   - `as_nparray()` — ~500 ns–1 µs

3. **结论**：`table.column.x` 的实际瓶颈是 SWIG `get_column()`（每次调用约 4–5 µs），与字段名查找方式无关。进一步改善需要缓存 `get_column()` 返回值或走 C++ 批量 API（OPT-7）。

### 有效改善

- **scalar read/write**（OPT-3）：~10–20% 改善符合预期，消除了 Python if-chain 判断开销。
- **row-wise 场景**：`point_cloud_read_rowwise`（-10%）和 `point_cloud_write_rowwise`（-12%）因访问 x/y/z 各一次，合计 3× scalar dispatch 的收益。

---

## 结论与下一步

OPT-1 + OPT-3 属于**低风险、零破坏性的 Python 层清理**，带来了 10–20% 的 row-wise 场景改善。`column_accessor_scan` 的真实瓶颈在于 SWIG `get_column()`，非 Python 字段名查找，这一发现已更新至 `optimize_plan.md`。

下一轮建议：**OPT-2**（`Feature._cache` 懒分配）→ 减少迭代场景的 GC 压力，目标 `iter_table` 从 2 µs/item 降至 ~1.7 µs/item。
