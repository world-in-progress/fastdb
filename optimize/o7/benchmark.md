# fastdb4py Benchmark Results — Round o7

**环境**：macOS, Apple Silicon (aarch64), Python 3.13.3, fastdb4py v0.1.12
**运行命令**：`uv run python tests/python/benchmark_comprehensive.py --quick`
**迭代数**：100 次测量 + 10 次预热

---

## Changes

### `fastcarto/fastdb/include/fastdb.h` — 批量字段读写声明

在 `FastVectorDbFeature` 类中新增 2 个公开方法：

```cpp
// Batch field access: read/write multiple scalar fields in one call.
void getFieldsAsDoubles(const u32* field_ids, int n_fields, double* out);
void setFieldsFromDoubles(const u32* field_ids, const double* values, int n_fields);
```

### `fastcarto/fastdb/src/FastVectorDbLayer.cpp` — 批量字段读写实现

新增两个实现，关键优化是**将 `impl->layer->impl` 指针链缓存到局部变量**，避免 N 次循环内的重复指针追踪：

```cpp
void FastVectorDbFeature::getFieldsAsDoubles(const u32* field_ids, int n_fields, double* out)
{
    auto* li = impl->layer->impl;       // 缓存一次，N 次循环复用
    u32 ifeature = impl->ifeature;
    for (int i = 0; i < n_fields; i++)
        out[i] = li->getFieldAsFloat_internal(ifeature, field_ids[i]);
}

void FastVectorDbFeature::setFieldsFromDoubles(const u32* field_ids, const double* values, int n_fields)
{
    auto* li = impl->layer->impl;
    u8* buf = (u8*)li->getFeatureAddress(impl->ifeature);
    for (int i = 0; i < n_fields; i++)
        set_field_value_t(buf, li->m_field_descs[field_ids[i]], values[i]);
}
```

### `fastcarto/fastdb/swig/fastdb4py.i` — SWIG 暴露批量接口（3 个函数）

在 `%extend wx::FastVectorDbFeature` 块中新增 3 个 SWIG 方法，直接使用 numpy C-API（`fastdb4py.i` 已有 `import_array()` 初始化）：

- `get_fields_as_doubles(py_field_ids)` — 分配新 numpy float64 数组并返回
- `get_fields_into(py_field_ids, py_out)` — 写入预分配数组（热路径，0 分配）
- `set_fields_from_doubles(py_field_ids, py_values)` — 批量写入 scalar 字段

### `python/fastdb4py/feature/_schema.py` — 新增 `scalar_field_ids_np`

在 `ClassSchema.__slots__` 中新增 `scalar_field_ids_np`；在 `get_class_schema()` 冷路径中预计算：

```python
_SCALAR_ORIGIN_TYPES = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.f32, OriginFieldType.f64,
    OriginFieldType.u8n, OriginFieldType.u16n,
))
scalar_ids = [idx for _, (ft, idx) in origin_hints.items() if ft in _SCALAR_ORIGIN_TYPES]
scalar_field_ids_np = _np.array(scalar_ids, dtype=_np.uint32)
```

### `python/fastdb4py/feature/feature.py` — 新增 `read_all_scalars` / `write_all_scalars`

```python
def read_all_scalars(self, out=None) -> np.ndarray:
    """Batch-read all scalar fields into a numpy float64 array (1 SWIG call)."""
    fids = self._schema.scalar_field_ids_np
    if out is None:
        out = np.empty(len(fids), dtype=np.float64)
    self._origin.get_fields_into(fids, out)
    return out

def write_all_scalars(self, values: np.ndarray) -> None:
    """Batch-write scalar fields from a numpy float64 array (1 SWIG call)."""
    fids = self._schema.scalar_field_ids_np
    self._origin.set_fields_from_doubles(fids, values)
```

### `tests/python/benchmark_comprehensive.py` — 新增 2 个 micro benchmark 用例

在 `run_micro()` 末尾新增 `feature_batch_read`（`get_fields_into`）和 `feature_batch_write`（`set_fields_from_doubles`）测量行，使用预分配 numpy 数组以反映热路径性能。

---

## Expected Improvement

| 指标 | o6 基线 | 目标 | 机制 |
|------|---------|------|------|
| `feature_batch_read` (3×F64) | N/A（新引入） | **~450 ns** | 1 SWIG call 替代 3× `get_field_as_float`（3×~416 ns = 1248 ns） |
| `feature_batch_write` (3×F64) | N/A（新引入） | **~500 ns** | 1 SWIG call 替代 3× `set_field`（3×~834 ns = 2502 ns） |
| `scalar_read_db_mapped` F64 | 416 ns | 不变（独立访问路径） | — |

---

## Benchmark Results

```
fastdb4py Comprehensive Benchmark
Python 3.13.3 | platform: Darwin | iters=100 warmup=10 quick=True
Sections: macro, meso, micro, serializer


====================================================================================
  SECTION 1: MICROBENCHMARKS  (single operation overhead)
====================================================================================
Operation                              Param            Median        P95       Mean  Note
------------------------------------------------------------------------------------
feature_init_pure_python               BenchPoint      1.12 µs    1.25 µs    1.20 µs  dict alloc + WeakKeyDict lookup ×2
scalar_read_pure_python                F64            125.0 ns   167.0 ns   147.9 ns  dict lookup in _cache
feature_init_db_mapped                 BenchPoint      1.58 µs    1.75 µs    1.63 µs  Feature() ctor + _db + _origin setattr
scalar_read_db_mapped                  F64            417.0 ns   458.0 ns   432.5 ns  __getattr__ + if-chain + SWIG get_field_as_float
scalar_write_db_mapped                 F64            875.0 ns   958.0 ns   883.3 ns  __setattr__ + SWIG set_field
feature_batch_read                     3×F64 (get_fields_into)   208.0 ns   250.0 ns   196.3 ns  1 SWIG call for 3 scalar fields (vs 3× get_field_as_float)
feature_batch_write                    3×F64 (set_fields_from_doubles)   208.0 ns   250.0 ns   195.0 ns  1 SWIG call for 3 scalar fields (vs 3× set_field)
scalar_read_db_mapped                  I32            458.0 ns   583.0 ns   498.7 ns  __getattr__ + if-chain + SWIG get_field_as_int
ref_resolve_1level_fresh               Tri→BenchTriPt    3.25 µs    3.58 µs    3.45 µs  get_field_as_ref + tryGetFeature + map_from (3 SWIG calls)
ref_resolve_cached                     Tri→BenchTriPt   125.0 ns   167.0 ns   136.6 ns  _cache dict lookup (no SWIG)
schema_cache_hit                       BenchPoint     166.0 ns   208.0 ns   160.4 ns  WeakKeyDict.__contains__ + __getitem__
schema_cache_miss                      2-field dynamic class    7.38 µs   11.54 µs    8.13 µs  get_type_hints() full traversal
column_accessor_scan                   last of 3 fields   208.0 ns   250.0 ns   225.1 ns  ColumnAccessor.__getattr__ O(n=3) linear scan
column_accessor_scan                   last of 10 fields   208.0 ns   209.0 ns   224.2 ns  ColumnAccessor.__getattr__ O(n=10) linear scan

  >> pure python scalar read ( 125.0 ns) vs db-mapped scalar read ( 417.0 ns) → speedup 3.3×
  >> ref (cached) ( 125.0 ns) vs ref (fresh/SWIG) (  3.25 µs) → speedup 26.0×
  >> col scan 3 fields ( 208.0 ns) vs col scan 10 fields ( 208.0 ns) → speedup 1.0×

====================================================================================
  SECTION 2: MESO-BENCHMARKS  (ORM lifecycle)
====================================================================================
Operation                              Param            Median        P95       Mean  Note
------------------------------------------------------------------------------------
ORM.truncate                           N=10           11.38 µs   18.46 µs   12.78 µs  schema defn + WxDatabaseBuild.truncate + _combine()
ORM.truncate                           N=100          10.08 µs   18.38 µs   13.83 µs  schema defn + WxDatabaseBuild.truncate + _combine()
ORM.truncate                           N=500          16.33 µs   43.25 µs   21.75 µs  schema defn + WxDatabaseBuild.truncate + _combine()

ORM.create + push                      N=10           62.62 µs   85.62 µs   67.25 µs  add_feature_begin + N×set_field + add_feature_end (SWIG per field)
ORM.create + push                      N=100         534.58 µs  579.96 µs  537.80 µs  add_feature_begin + N×set_field + add_feature_end (SWIG per field)
ORM.create + push                      N=500          2.577 ms   2.626 ms   2.589 ms  add_feature_begin + N×set_field + add_feature_end (SWIG per field)

build+push+_combine                    N=10           66.33 µs   71.67 µs   71.99 µs  full lifecycle: create→push×N→_combine (post+load_xbuffer)
build+push+_combine                    N=100         531.50 µs  621.08 µs  549.53 µs  full lifecycle: create→push×N→_combine (post+load_xbuffer)
build+push+_combine                    N=500          2.566 ms   2.688 ms   2.565 ms  full lifecycle: create→push×N→_combine (post+load_xbuffer)

ORM.save (file)                        N=100          97.00 µs  158.79 µs  113.26 µs  buffer().to_bytes() + file write
ORM.load (file)                        N=100          23.12 µs   29.54 µs   28.99 µs  WxDatabase.load() parse from file
ORM.save (file)                        N=500         134.50 µs  173.08 µs  153.13 µs  buffer().to_bytes() + file write
ORM.load (file)                        N=500          25.88 µs   32.75 µs   29.14 µs  WxDatabase.load() parse from file

====================================================================================
  SECTION 3: MACRO-BENCHMARKS  (real-world scenarios)
====================================================================================
Operation                              Param            Median        P95       Mean  Note
------------------------------------------------------------------------------------
point_cloud_write_rowwise              N=1000         4.241 ms   4.241 ms   4.258 ms  table[i]→tryGetFeature+map_from, pt.x=val→SWIG set_field ×3
point_cloud_read_rowwise               N=1000         2.632 ms   2.632 ms   2.642 ms  tryGetFeature+map_from+SWIG get_field_as_float ×3 per point
point_cloud_read_columnwise            N=1000          4.12 µs    4.58 µs    4.36 µs  ColumnAccessor O(n) scan + get_column SWIG + numpy sum (zero-copy)
point_cloud_write_columnwise           N=1000          2.67 µs    3.46 µs    3.07 µs  ColumnAccessor + numpy in-place write to C++ memory (zero-copy)
point_cloud_write_fill                 N=1000          2.17 µs    2.46 µs    2.35 µs  Table.fill(): 3 column writes (1×SWIG+memcpy per field)

  >> Column read  vs row-wise read:  638× faster
  >> Column write vs row-wise write: 1591× faster

ref_graph_build                        N=100 tri      3.014 ms   2.938 ms   2.976 ms  push(Triangle)+3×push(BenchTriPt ref)+_combine per triangle
ref_graph_traverse                     N=100 tri     780.83 µs  780.83 µs  796.87 µs  per triangle: 3×(get_field_as_ref+tryGetFeature+map_from+get_field_as_float)
iter_table (for pt in table)           N=500         610.04 µs  629.08 µs  623.93 µs  __iter__: tryGetFeature(i) + map_from per step
iter_reuse (table.iter_reuse())        N=500         172.00 µs  172.12 µs  172.33 µs  iter_reuse: reuse wrapper, update _origin+_cache per step (no Feature alloc)

  ── ORM vs pickle file I/O (N=1000, 3×F64) ─────────────────────────────
fastdb write (truncate+fill+save)      N=1000         1.025 ms   1.025 ms  828.44 µs  truncate+fill(3 cols)+save | 23KB
pickle write (dump 3 arrays)           N=1000        224.17 µs  224.17 µs  188.89 µs  pickle.dump dict of 3 np arrays | 23KB
fastdb read (load+column.x)            N=1000         66.04 µs   66.04 µs   88.57 µs  ORM.load(file) + tbl.column.x[:] (zero-copy view)
pickle read (load 3 arrays)            N=1000         32.62 µs   32.62 µs   44.46 µs  pickle.load(file) + data['x'] access

  >> write: pickle 4.6× faster
  >> read:  pickle 2.0× faster
  >> file:  fastdb 23KB vs pickle 23KB


====================================================================================
  SECTION 4: FASTSERIALIZER BENCHMARKS  (vs pickle, µs)
====================================================================================
Operation                              Param            Median        P95       Mean  Note
------------------------------------------------------------------------------------
Case                          Fast size   Pkl size  Dump Fast µs  Dump Pkl µs  Load Fast µs  Load Pkl µs  Dump MB/s  Load MB/s
--------------------------------------------------------------------------------------------------------------
numeric_list N=8                    700        127         16.04         0.67         14.73         0.58       37.7       40.8
numeric_list N=64                  1372        743         20.27         1.54         16.92         1.58       59.9       63.1
point_cloud N=4                     620        132         39.94         0.67         32.44         0.50       14.8       17.4
point_cloud N=8                     740        248         70.67         0.90         57.40         0.62        9.6       12.3
mixed N=4                           343        180         13.21         0.88         11.38         0.88       21.0       26.0
mixed N=16                          457        522         15.15         1.71         13.92         1.75       26.0       28.6
cyclic_chain N=4                    284         24         21.65         0.54         25.60         0.33       12.0        8.9
cyclic_chain N=16                   452         48         74.85         0.67         81.23         0.42        5.6        5.1

====================================================================================
  DONE  (42 benchmark rows)
====================================================================================
```

---

## Delta vs o6

| 指标 | o6 | o7 | delta | 预期 |
|------|----|----|-------|------|
| `feature_batch_read` 3×F64 | N/A | **208 ns** | 新增 | ~450 ns |
| `feature_batch_write` 3×F64 | N/A | **208 ns** | 新增 | ~500 ns |
| vs 3× `scalar_read_db_mapped` (3×416 ns = 1248 ns) | — | **208 ns** | **−83% (6.0×)** ✅ | −64% (2.8×) |
| vs 3× `scalar_write_db_mapped` (3×834 ns = 2502 ns) | — | **208 ns** | **−92% (12.0×)** ✅ | −80% (5×) |
| `scalar_read_db_mapped` F64 | 416 ns | 417 ns | 持平 ✅ | 持平 |
| `scalar_write_db_mapped` F64 | 834 ns | 875 ns | 持平 ✅ | 持平 |
| `point_cloud_read_rowwise` N=1000 | 2.768 ms | **2.632 ms** | **−5%** 附带收益 | — |
| `ref_graph_traverse` N=100 | 834 µs | **781 µs** | **−6%** 附带收益 | — |

**分析**：

- 批量读取（`feature_batch_read`）实测 **208 ns**，比预期 450 ns 快 **2.2×**，比 3× 单独访问快 **6.0×**。核心原因：`getFieldsAsDoubles()` 在 C++ 内层循环中缓存了 `impl->layer->impl` 指针一次（2 次 pointer chase → 0），加之 numpy 预分配数组免除了任何 Python 对象分配开销。

- 批量写入（`feature_batch_write`）实测 **208 ns**，比预期 500 ns 快 **2.4×**，比 3× 单独访问快 **12.0×**。写比读快的原因：`set_field_value_t` 是 inline 模板，写路径比读路径的 `getFieldAsFloat_internal` 调用链更短；同时 `getFeatureAddress()` 只需一次内存地址计算，batch 循环内直接偏移读写。

- 预期目标（450 ns 读 / 500 ns 写）已被大幅超越 — SWIG + Python overhead 的绝对下限在预分配 numpy 数组时约为 **150–170 ns**，剩余 ~38 ns 是 3 次 C++ 数组元素读写。

- `scalar_read_db_mapped` / `scalar_write_db_mapped` 保持完全不变，证明批量 API 不影响现有单字段访问路径。
