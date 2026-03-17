# fastdb4py Benchmark Results — Round o5

**环境**：macOS, Apple Silicon (aarch64), Python 3.13.3, fastdb4py v0.1.12
**运行命令**：`uv run python tests/python/benchmark_comprehensive.py --quick`
**迭代数**：100 次测量 + 10 次预热

---

## Changes

### OPT-6：统一 ClassSchema — 将 4 个 WeakKeyDictionary 合并为 1 个

**问题**：`Feature.__init__` 每次调用时发起 **2 次 WeakKeyDict 查找**（Cache 1: `_feature_hints_cache` → `get_type_hints()` 结果；Cache 2: `_global_feature_defn_cache` → 字段类型映射）。此外 `serializer._get_class_schema()`（Cache 4）在 miss 时重复调用 `get_type_hints()` + `get_all_defns()`，`_create_column_accessor()`（Cache 3）同样独立维护 WeakKeyDict cache。

**方案**：新建 `ClassSchema` 数据类（`__slots__`）+ 统一 `_SCHEMA_CACHE: WeakKeyDictionary`，4 个分散的 cache 合并为 1 个。

#### 新文件：`python/fastdb4py/feature/_schema.py`

新建 `ClassSchema` 数据类，包含：
- `hints` — `get_type_hints()` 结果（原 Cache 1）
- `origin_hints` — `parse_defns()` 字段类型映射（原 Cache 2）
- `ordered_defns` — 按字段索引排序的 `(name, OriginFieldType)` 列表
- `field_index_map` — name → column 位置（原 Cache 3 消费的数据）
- `column_accessor_class` — 懒加载 ColumnAccessor 类（原 Cache 3 值）

单次 `get_type_hints()` 调用构建完整 `ClassSchema`，之后所有消费方读同一对象。

#### 修改：`python/fastdb4py/feature/feature.py`

删除 `_feature_hints_cache`/`_feature_hints_cache_lock`/`_get_feature_hints()`，`__init__` 改为单次查找：
```python
# 旧（2× WeakKeyDict）
self._type_hints    = _get_feature_hints(self.__class__)   # Cache 1
self._origin_hints  = parse_defns(self.__class__)          # Cache 2（→ Cache 2 WeakKeyDict）

# 新（1× WeakKeyDict）
_schema = get_class_schema(self.__class__)
self._type_hints   = _schema.hints
self._origin_hints = _schema.origin_hints
```

同时补回缺失的 `from .base import BaseFeature`（OPT-6 改动中误删，测试发现并修复）。

#### 修改：`python/fastdb4py/feature/utils.py`

删去独立 WeakKeyDict + Lock，`parse_defns`/`get_all_defns` 改为代理 `ClassSchema`：
```python
def parse_defns(cls):     return get_class_schema(cls).origin_hints
def get_all_defns(cls):   return get_class_schema(cls).ordered_defns
```

#### 修改：`python/fastdb4py/feature/__init__.py`

新增导出 `get_class_schema`。

#### 修改：`python/fastdb4py/orm/table.py`

删除 `_column_accessor_cache` WeakKeyDictionary + `_column_accessor_cache_lock`，改用 `schema.column_accessor_class` 字段作为缓存载体：
```python
schema = get_class_schema(feature_type)
ColumnAccessorClass = schema.column_accessor_class   # 读
# ...（miss 分支创建类后）
schema.column_accessor_class = ColumnAccessor        # 写（替换 WeakKeyDict[cls] = 写）
```

`_field_index_map` 直接从 `schema.field_index_map` 读取，消除冷路径中的 `get_all_defns()` 调用。

#### 修改：`python/fastdb4py/serializer.py`

`_get_class_schema()` 读 ClassSchema 而非重新计算：
```python
base  = _get_unified_schema(cls)   # 零成本：WeakKeyDict 热路径
hints = base.hints                 # 无需 get_type_hints()
defns = base.ordered_defns         # 无需 get_all_defns()
# 仅计算 serializer-specific 部分（numeric_field_kinds, db_field_index_by_schema）
```

---

## Expected Improvement

| 指标 | o4 基线 | 目标 | 机制 |
|------|---------|------|------|
| `feature_init_pure_python` | 1.83 µs | ~1.55 µs (−15%) | `__init__` from 2× → 1× WeakKeyDict |
| `feature_init_db_mapped` | 2.42 µs | ~2.1 µs (−13%) | 同上 |
| `schema_cache_miss` | 7.29 µs | ~3.5–4 µs (−2×) | 消除重复 `get_type_hints()` 调用 |

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
feature_init_pure_python               BenchPoint      1.25 µs    1.42 µs    1.37 µs  dict alloc + WeakKeyDict lookup ×2
scalar_read_pure_python                F64            125.0 ns   167.0 ns   135.3 ns  dict lookup in _cache
feature_init_db_mapped                 BenchPoint      1.71 µs    1.88 µs    1.83 µs  Feature() ctor + _db + _origin setattr
scalar_read_db_mapped                  F64            416.0 ns   459.0 ns   420.8 ns  __getattr__ + if-chain + SWIG get_field_as_float
scalar_write_db_mapped                 F64            834.0 ns   916.0 ns   867.9 ns  __setattr__ + SWIG set_field
scalar_read_db_mapped                  I32            417.0 ns   541.0 ns   474.6 ns  __getattr__ + if-chain + SWIG get_field_as_int
ref_resolve_1level_fresh               Tri→BenchTriPt    3.33 µs    3.62 µs    3.43 µs  get_field_as_ref + tryGetFeature + map_from (3 SWIG calls)
ref_resolve_cached                     Tri→BenchTriPt   125.0 ns   167.0 ns   130.8 ns  _cache dict lookup (no SWIG)
schema_cache_hit                       BenchPoint     209.0 ns   250.0 ns   232.5 ns  WeakKeyDict.__contains__ + __getitem__
schema_cache_miss                      2-field dynamic class    6.17 µs    9.21 µs    6.57 µs  get_type_hints() full traversal
column_accessor_scan                   last of 3 fields   208.0 ns   250.0 ns   253.2 ns  ColumnAccessor.__getattr__ O(n=3) linear scan
column_accessor_scan                   last of 10 fields   167.0 ns   209.0 ns   199.5 ns  ColumnAccessor.__getattr__ O(n=10) linear scan

  >> pure python scalar read ( 125.0 ns) vs db-mapped scalar read ( 416.0 ns) → speedup 3.3×
  >> ref (cached) ( 125.0 ns) vs ref (fresh/SWIG) (  3.33 µs) → speedup 26.7×
  >> col scan 3 fields ( 208.0 ns) vs col scan 10 fields ( 167.0 ns) → speedup 0.8×

====================================================================================
  SECTION 2: MESO-BENCHMARKS  (ORM lifecycle)
====================================================================================
Operation                              Param            Median        P95       Mean  Note
------------------------------------------------------------------------------------
ORM.truncate                           N=10           11.67 µs   14.62 µs   15.57 µs  schema defn + WxDatabaseBuild.truncate + _combine()
ORM.truncate                           N=100          11.33 µs   22.21 µs   13.02 µs  schema defn + WxDatabaseBuild.truncate + _combine()
ORM.truncate                           N=500          14.12 µs   21.71 µs   15.18 µs  schema defn + WxDatabaseBuild.truncate + _combine()

ORM.create + push                      N=10           66.08 µs   82.62 µs   71.12 µs  add_feature_begin + N×set_field + add_feature_end (SWIG per field)
ORM.create + push                      N=100         528.33 µs  561.42 µs  539.30 µs  add_feature_begin + N×set_field + add_feature_end (SWIG per field)
ORM.create + push                      N=500          2.612 ms   2.657 ms   2.616 ms  add_feature_begin + N×set_field + add_feature_end (SWIG per field)

build+push+_combine                    N=10           66.58 µs   72.21 µs   70.57 µs  full lifecycle: create→push×N→_combine (post+load_xbuffer)
build+push+_combine                    N=100         541.08 µs  559.50 µs  550.06 µs  full lifecycle: create→push×N→_combine (post+load_xbuffer)
build+push+_combine                    N=500          2.635 ms   2.736 ms   2.648 ms  full lifecycle: create→push×N→_combine (post+load_xbuffer)

ORM.save (file)                        N=100          69.79 µs  119.96 µs   79.21 µs  buffer().to_bytes() + file write
ORM.load (file)                        N=100          26.04 µs   31.88 µs   36.26 µs  WxDatabase.load() parse from file
ORM.save (file)                        N=500          66.75 µs  172.00 µs   93.95 µs  buffer().to_bytes() + file write
ORM.load (file)                        N=500          26.75 µs   31.92 µs   35.05 µs  WxDatabase.load() parse from file

====================================================================================
  SECTION 3: MACRO-BENCHMARKS  (real-world scenarios)
====================================================================================
Operation                              Param            Median        P95       Mean  Note
------------------------------------------------------------------------------------
point_cloud_write_rowwise              N=1000         4.473 ms   4.473 ms   4.441 ms  table[i]→tryGetFeature+map_from, pt.x=val→SWIG set_field ×3
point_cloud_read_rowwise               N=1000         3.165 ms   3.165 ms   4.632 ms  tryGetFeature+map_from+SWIG get_field_as_float ×3 per point
point_cloud_read_columnwise            N=1000          4.33 µs    5.50 µs    5.25 µs  ColumnAccessor O(n) scan + get_column SWIG + numpy sum (zero-copy)
point_cloud_write_columnwise           N=1000          2.67 µs    2.88 µs    2.76 µs  ColumnAccessor + numpy in-place write to C++ memory (zero-copy)
point_cloud_write_fill                 N=1000          2.08 µs    2.33 µs    2.20 µs  Table.fill(): 3 column writes (1×SWIG+memcpy per field)

  >> Column read  vs row-wise read:  730× faster
  >> Column write vs row-wise write: 1677× faster

ref_graph_build                        N=100 tri      3.302 ms   3.071 ms   3.186 ms  push(Triangle)+3×push(BenchTriPt ref)+_combine per triangle
ref_graph_traverse                     N=100 tri     853.04 µs  853.04 µs  857.11 µs  per triangle: 3×(get_field_as_ref+tryGetFeature+map_from+get_field_as_float)
iter_table (for pt in table)           N=500         667.96 µs  740.92 µs  688.28 µs  __iter__: tryGetFeature(i) + map_from per step
iter_reuse (table.iter_reuse())        N=500         176.96 µs  177.42 µs  175.26 µs  iter_reuse: reuse wrapper, update _origin+_cache per step (no Feature alloc)

  ── ORM vs pickle file I/O (N=1000, 3×F64) ─────────────────────────────
fastdb write (truncate+fill+save)      N=1000        134.67 µs  134.67 µs  164.65 µs  truncate+fill(3 cols)+save | 23KB
pickle write (dump 3 arrays)           N=1000        134.96 µs  134.96 µs  159.14 µs  pickle.dump dict of 3 np arrays | 23KB
fastdb read (load+column.x)            N=1000         72.92 µs   72.92 µs   98.78 µs  ORM.load(file) + tbl.column.x[:] (zero-copy view)
pickle read (load 3 arrays)            N=1000         43.12 µs   43.12 µs   72.81 µs  pickle.load(file) + data['x'] access

  >> write: fastdb 1.0× faster
  >> read:  pickle 1.7× faster
  >> file:  fastdb 23KB vs pickle 23KB


====================================================================================
  SECTION 4: FASTSERIALIZER BENCHMARKS  (vs pickle, µs)
====================================================================================
Operation                              Param            Median        P95       Mean  Note
------------------------------------------------------------------------------------
Case                          Fast size   Pkl size  Dump Fast µs  Dump Pkl µs  Load Fast µs  Load Pkl µs  Dump MB/s  Load MB/s
--------------------------------------------------------------------------------------------------------------
numeric_list N=8                    700        127         16.15         0.71         15.10         0.58       36.9       39.4
numeric_list N=64                  1372        743         20.98         1.46         15.25         1.42       55.4       75.7
point_cloud N=4                     620        132         44.21         0.67         36.00         0.46       12.6       15.1
point_cloud N=8                     740        248         66.10         0.83         51.17         0.62       10.7       13.6
mixed N=4                           343        180         13.62         0.85         11.77         0.88       22.5       24.5
mixed N=16                          457        522         15.58         1.67         14.02         1.79       23.8       27.0
cyclic_chain N=4                    284         24         22.40         0.54         23.12         0.33       11.5       10.8
cyclic_chain N=16                   452         48         67.92         0.62         77.58         0.42        6.3        5.7

====================================================================================
  DONE  (40 benchmark rows)
====================================================================================
```

---

## Delta vs o4

| 指标 | o4 | o5 | delta | 预期 |
|------|----|----|-------|------|
| `feature_init_pure_python` | 1.83 µs | **1.25 µs** | **−32%** ✅ | −15% |
| `feature_init_db_mapped` | 2.42 µs | **1.71 µs** | **−29%** ✅ | −13% |
| `schema_cache_miss` | 7.29 µs | **6.17 µs** | **−15%** ⚠️ | −50% |
| `schema_cache_hit` | ~209 ns | 209 ns | 持平 | — |
| ORM.truncate N=100 | ~11 µs | 11.33 µs | 持平 | — |
| point_cloud_write_fill | 2.75 µs | **2.08 µs** | **−24%** ✅ | — |

**分析**：
- `feature_init` 两个指标均超出预期（−29~32% vs −13~15%）。原因：去除了 Cache 1 的整个 WeakKeyDict + Lock 对象（不只节省一次查找），同时 `parse_defns()` 调用链也完全消除（之前 Cache 2 fallback 还有 Lock 获取开销）。
- `schema_cache_miss` 改善幅度 (−15%) 低于预期 (−50%)。原因：cold path 仍需调用一次 `get_type_hints()`（跨继承树遍历），这是主要耗时；之前重复调用的第二次 `get_type_hints()`（在 `serializer._get_class_schema()` 中）被消除，但该路径在 `schema_cache_miss` benchmark（直接创建 Feature 子类）中不触发，因此统计上看不到 2× 改善。
- `point_cloud_write_fill` 从 2.75 µs → 2.08 µs（−24%），因为首次 ColumnAccessor 访问的冷路径节省了旧的 Cache 3 WeakKeyDict lookup + `get_all_defns()` 调用。
