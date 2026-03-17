# fastdb4py Benchmark Results — Round o2

**环境**：macOS, Apple Silicon (aarch64), Python 3.13.3, fastdb4py v0.1.12
**运行命令**：`uv run python tests/python/benchmark_comprehensive.py --quick`
**迭代数**：100 次测量 + 10 次预热

---

## Changes

### `python/fastdb4py/feature/feature.py` — OPT-2: `Feature._cache` 懒分配

**问题**：`Feature.__init__` 始终分配一个空 `dict` 作为 `_cache`（原 `L43: self._cache = {}`），即使对于 db-mapped read-only Feature（`map_from()` 返回的对象），该 dict 永远不会被写入 scalar 值（scalar 字段直接走 SWIG）。

**改动**：
1. `_cache` 初始化改为 `None`（懒分配）
2. 新增 `_get_cache()` 辅助方法：首次调用时分配 dict，后续复用
3. `__init__` kwargs 路径：仅在 kwargs 非空时才分配 dict，并一次性整体赋值
4. `__getattr__` 读路径：`if name in self._cache` → `cache = self._cache; if cache is not None and name in cache`
5. 所有写路径（7 处 `__getattr__`/`__setattr__` 写 cache）改为 `self._get_cache()[name] = ...`

### `python/fastdb4py/serializer.py` — 配套修改

`loads()` 中 `obj._cache[fn] = val`（6 处）→ `obj._get_cache()[fn] = val`

---

## Expected Improvement

| 指标 | o1.2 基线 | 预期目标 | 预期改善 |
|------|-----------|----------|----------|
| `feature_init_db_mapped` | 2.54 µs | ~1.3–1.5 µs | **~1.7–2×** |
| `iter_table` per item | ~2.04 µs | ~1.0–1.3 µs | **~1.5–2×** |
| `feature_init_pure_python`（无 kwargs） | 1.75 µs | ~0.7–1.0 µs | ~1.8× |

---

## Section 1: Microbenchmarks（单操作开销）

| 操作 | o0 Median | o1.2 Median | o2 Median | o0→o2 改善 |
|------|-----------|-------------|-----------|-----------|
| `feature_init_pure_python` | 1.79 µs | 1.75 µs | 1.83 µs | — |
| `scalar_read_pure_python` (F64) | 167 ns | 167 ns | 167 ns | — |
| `feature_init_db_mapped` | 2.50 µs | 2.54 µs | **2.50 µs** | — |
| `scalar_read_db_mapped` (F64) | 667 ns | 542 ns | 541 ns | ↑19% |
| `scalar_write_db_mapped` | 1.29 µs | 1.17 µs | 1.17 µs | ↑9% |
| `ref_resolve_cached` | 167 ns | 167 ns | 167 ns | — |
| `column_accessor_scan` (3f) | 8.67 µs | 250 ns | 250 ns | ↑35× |
| `column_accessor_scan` (10f) | 9.54 µs | 250 ns | 250 ns | ↑38× |

---

## Section 3: Macro-benchmarks（真实场景）

| 操作 | o0 Median | o1.2 Median | o2 Median | o1.2→o2 改善 |
|------|-----------|-------------|-----------|-------------|
| `point_cloud_read_rowwise` | 4.40 ms | 4.06 ms | 3.92 ms | ↑3.5% |
| `point_cloud_read_columnwise` | 33.58 µs | 5.58 µs | 5.50 µs | — |
| **`iter_table`** (N=500) | 1.01 ms | 1.021 ms | **978 µs** | **↑4.3%** |

**`iter_table` 折算每 item**：1.021 ms / 500 = 2.04 µs → 978 µs / 500 = **1.96 µs**（节省 ~80 ns/item）

---

## Delta 分析

### 实际 vs 预期

| 指标 | 预期改善 | 实际改善 | 评估 |
|------|----------|----------|------|
| `feature_init_db_mapped` | ~1.7–2× | **无明显改善（2.54→2.50 µs）** | ✗ 远低于预期 |
| `iter_table` per item | ~1.5–2× | **~4%（2.04→1.96 µs）** | ✗ 远低于预期 |

### 根本原因：Python 3.13 dict 空对象极其廉价

预期 "dict alloc ~1 µs" 的假设在 Python 3.13 / aarch64 上**不成立**：

CPython 3.13 为小 dict 维护了**对象 free-list**，`{}` 创建操作在 free-list 命中时约 **50–100 ns**（而非 1–1.5 µs）。实测改善量约 80 ns/item（与这个估算一致），说明 dict alloc 本身只有 ~80 ns。

### 真实开销分析（`feature_init_db_mapped`）

| 步骤 | 估算耗时 |
|------|----------|
| `cls()` 函数调用开销 | ~100 ns |
| `self._cache = None`（object.__setattr__） | ~180 ns |
| `self._origin = None`（同上） | ~180 ns |
| `self._db = None`（同上） | ~180 ns |
| `_get_feature_hints(cls)` WeakKeyDict 查找 | ~300 ns |
| `parse_defns(cls)` WeakKeyDict 查找 | ~300 ns |
| `if kwargs:` + 空分支 | ~10 ns |
| `map_from`: `feature._db = db` + `feature._origin = origin` | ~400 ns |
| 合计 | **~1.65–2.5 µs** |

**结论**：真实瓶颈是 **3× `object.__setattr__` + 2× WeakKeyDict 查找**，约 1.3 µs，与改动无关。OPT-6（统一 ClassSchema cache，将 2× WeakKeyDict → 1×）才是 `feature_init` 的有效优化方向。

### 代码正确性

改动后所有 10 个测试全通过。`_get_cache()` 的懒分配逻辑对所有路径（纯 Python、db-mapped、serializer deserialization）均正确工作。代码架构更清晰（写路径统一通过 `_get_cache()`）。

---

## 结论与下一步

OPT-2 的实际 GC/内存收益很小（dict free-list 掩盖了分配开销），但代码重构本身是有价值的（明确了 `_cache` 的写路径语义，从 `_get_cache()` 追踪所有写入更容易）。

下一轮推荐：**OPT-4**（`Table.iter_reuse()`）或 **OPT-6**（统一 ClassSchema）：
- OPT-4：新增 `iter_reuse()` 方法，复用同一 Feature 实例，彻底消除迭代中的 Feature 对象分配开销（~2 µs → ~0.8 µs/item）
- OPT-6：合并 4 个 WeakKeyDictionary → 1 次 `get_type_hints()` 调用，改善 `feature_init` 和 `schema_cache_miss`
