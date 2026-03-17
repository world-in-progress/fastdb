# fastdb4py Benchmark Results

**环境**：macOS, Apple Silicon (aarch64), Python 3.13.3, fastdb4py v0.1.12
**运行命令**：`uv run python tests/python/benchmark_comprehensive.py --quick`
**迭代数**：100 次测量 + 10 次预热

---

## Section 1: Microbenchmarks（单操作开销）

| 操作 | Median | P95 | Mean | 瓶颈说明 |
|------|--------|-----|------|----------|
| `feature_init_pure_python` | 1.79 µs | 2.08 µs | 2.25 µs | dict alloc + WeakKeyDict lookup ×2 |
| `scalar_read_pure_python` (F64) | 167 ns | 209 ns | 169 ns | dict lookup in `_cache` |
| `feature_init_db_mapped` | 2.50 µs | 2.75 µs | 2.62 µs | Feature() ctor + `_db` + `_origin` setattr |
| `scalar_read_db_mapped` (F64) | 667 ns | 791 ns | 713 ns | `__getattr__` + if-chain + SWIG `get_field_as_float` |
| `scalar_write_db_mapped` (F64) | 1.29 µs | 1.38 µs | 1.35 µs | `__setattr__` + SWIG `set_field` |
| `scalar_read_db_mapped` (I32) | 625 ns | 791 ns | 686 ns | `__getattr__` + if-chain + SWIG `get_field_as_int` |
| `ref_resolve_1level_fresh` | 5.04 µs | 5.42 µs | 5.18 µs | 3× SWIG：`get_field_as_ref` + `tryGetFeature` + `map_from` |
| `ref_resolve_cached` | 167 ns | 250 ns | 207 ns | `_cache` dict lookup（无 SWIG） |
| `schema_cache_hit` | 291 ns | 334 ns | 294 ns | `WeakKeyDict.__contains__` + `__getitem__` |
| `schema_cache_miss` | 7.25 µs | 10.33 µs | 7.89 µs | `get_type_hints()` 完整遍历 |
| `column_accessor_scan` (last of 3) | 8.67 µs | 14.25 µs | 9.88 µs | `ColumnAccessor.__getattr__` O(n=3) 线性扫描 |
| `column_accessor_scan` (last of 10) | 9.54 µs | 12.00 µs | 10.10 µs | `ColumnAccessor.__getattr__` O(n=10) 线性扫描 |

**关键对比**：
- Pure Python scalar read vs DB-mapped：**4×** 差距（167 ns vs 667 ns）
- Ref resolve fresh vs cached：**30×** 差距（5.04 µs vs 167 ns）
- ColumnAccessor scan 3字段 vs 10字段：仅 **1.1×**（O(n) 增长被掩盖，绝对值已高达 ~9 µs）

---

## Section 2: Meso-benchmarks（ORM 生命周期）

| 操作 | Param | Median | P95 | Mean | 备注 |
|------|-------|--------|-----|------|------|
| `ORM.truncate` | N=10 | 14.25 µs | 27.33 µs | 22.27 µs | schema defn + truncate + `_combine()` |
| `ORM.truncate` | N=100 | 16.08 µs | 27.71 µs | 19.66 µs | |
| `ORM.truncate` | N=500 | 18.21 µs | 19.08 µs | 19.65 µs | |
| `ORM.create + push` | N=10 | 83.46 µs | 99.29 µs | 91.57 µs | 8.35 µs/feature |
| `ORM.create + push` | N=100 | 749 µs | 827 µs | 762 µs | 7.49 µs/feature |
| `ORM.create + push` | N=500 | 3.76 ms | 4.26 ms | 4.75 ms | 7.52 µs/feature |
| `build+push+_combine` | N=10 | 92.58 µs | 110 µs | 98.12 µs | `_combine()` ≈ 9 µs overhead |
| `build+push+_combine` | N=100 | 751 µs | 789 µs | 764 µs | |
| `build+push+_combine` | N=500 | 3.70 ms | 3.72 ms | 3.69 ms | |
| `ORM.save (file)` | N=100 | 64.21 µs | 85.17 µs | 73.75 µs | `buffer().to_bytes()` + file write |
| `ORM.load (file)` | N=100 | 33.17 µs | 37.83 µs | 36.36 µs | `WxDatabase.load()` parse |
| `ORM.save (file)` | N=500 | 83.08 µs | 105.58 µs | 86.38 µs | |
| `ORM.load (file)` | N=500 | 32.21 µs | 36.79 µs | 35.95 µs | |

**结论**：
- `truncate` 对 N 几乎不敏感（14–18 µs），说明 `_combine()` 的 post+reload 固定开销主导
- `push` 均摊约 **7.5 µs/feature**，主要由 SWIG 调用数决定（3个 scalar field = 5 SWIG calls/feature）
- `save/load` 文件延迟较低（33–83 µs），load 几乎与 N 无关（说明 parse 快）

---

## Section 3: Macro-benchmarks（真实场景）

| 操作 | Param | Median | Per-unit | 备注 |
|------|-------|--------|----------|------|
| `point_cloud_write_rowwise` | N=1000 | 6.64 ms | 6.64 µs/point | `table[i]`+`pt.x=`+`pt.y=`+`pt.z=` |
| `point_cloud_read_rowwise` | N=1000 | 4.40 ms | 4.40 µs/point | `table[i].x + .y + .z` |
| `point_cloud_read_columnwise` | N=1000 | 33.58 µs | 33.6 ns/point | numpy zero-copy sum |
| `point_cloud_write_columnwise` | N=1000 | 12.75 µs | 12.8 ns/point | numpy in-place write |
| `ref_graph_build` | N=100 tri | 4.44 ms | 44.4 µs/tri | push + 3× nested BenchTriPt |
| `ref_graph_traverse` | N=100 tri | 1.24 ms | 12.4 µs/tri | `tri.a.x + tri.b.y + tri.c.z` |
| `iter_table` | N=500 | 1.01 ms | 2.02 µs/item | `for pt in table`：tryGetFeature + map_from |

**关键速比**：

| 对比 | 速比 |
|------|------|
| Column read vs row-wise read | **131×** |
| Column write vs row-wise write | **521×** |
| Ref resolve (cached) vs fresh | **30×** |

---

## Section 4: FastSerializer vs pickle

| Case | Fast Size | Pickle Size | Dump Fast µs | Dump Pickle µs | Load Fast µs | Load Pickle µs | Dump MB/s | Load MB/s |
|------|-----------|-------------|-------------|----------------|-------------|----------------|-----------|-----------|
| numeric_list N=8 | 700 B | 127 B | 21.33 | 0.92 | 19.44 | 0.75 | 29.3 | 31.3 |
| numeric_list N=64 | 1372 B | 743 B | 27.15 | 1.92 | 20.60 | 1.88 | 42.5 | 56.6 |
| point_cloud N=4 | 620 B | 132 B | 50.77 | 0.83 | 40.77 | 0.60 | 11.1 | 12.8 |
| point_cloud N=8 | 740 B | 248 B | 88.15 | 1.10 | 69.06 | 0.88 | 8.2 | 9.9 |
| mixed N=4 | 343 B | 180 B | 17.06 | 1.10 | 15.29 | 1.25 | 15.9 | 18.7 |
| mixed N=16 | 457 B | 522 B | 19.62 | 2.08 | 18.62 | 2.33 | 18.9 | 20.7 |
| cyclic_chain N=4 | 284 B | 24 B | 28.27 | 0.75 | 30.77 | 0.42 | 9.4 | 8.2 |
| cyclic_chain N=16 | 452 B | 48 B | 88.75 | 0.79 | 104.08 | 0.54 | 4.7 | 4.2 |

**结论**：
- FastSerializer 比 pickle 慢约 **20–100×**，这是因为每次 dumps/loads 都需构建/拆解完整的 C++ 数据库（WxDatabaseBuild + post + load_xbuffer）
- numeric_list（columnar 路径）最快（~25–27 µs），因为 `List[U32]/List[F64]` 走 auxiliary layer 而非 blob
- cyclic_chain 随 N 增长显著（4→16 节点从 28µs 增至 88µs），BFS 遍历 + SWIG 逐节点解析是主因

---

## 已发现 Bug

**FastSerializer SIGBUS on `List[Feature]` N≥32**：`_ser_case_point_cloud` 在 N=32 时进程以 exit code 138（SIGBUS）崩溃。详见 `plan.md`。
