# fastdb4py 优化全景计划

> 基线数据来源：[o0/benchmark.md](o0/benchmark.md)（macOS, Apple Silicon, Python 3.13.3, fastdb4py v0.1.12）
> 代码分析来源：`plan.md`

---

## 基线关键数字速查

| 操作 | 基线延迟 | 对应 benchmark 指标 |
|------|----------|---------------------|
| Pure Python scalar read | 167 ns | `scalar_read_pure_python` |
| DB-mapped scalar read (F64) | 667 ns | `scalar_read_db_mapped` |
| DB-mapped scalar write | 1.29 µs | `scalar_write_db_mapped` |
| Feature 构造（db-mapped） | 2.50 µs | `feature_init_db_mapped` |
| ColumnAccessor field scan | **~9 µs** | `column_accessor_scan` |
| Ref resolve（首次） | 5.04 µs | `ref_resolve_1level_fresh` |
| Ref resolve（缓存命中） | 167 ns | `ref_resolve_cached` |
| Schema cache hit | 291 ns | `schema_cache_hit` |
| Schema cache miss | 7.25 µs | `schema_cache_miss` |
| `iter_table` per item | 2.02 µs | `iter_table` |
| `push` per feature (3×F64) | 7.5 µs | `ORM.create + push` |
| Point cloud row-wise read | 4.4 µs/pt | `point_cloud_read_rowwise` |
| Point cloud column-wise read | 33 ns/pt | `point_cloud_read_columnwise` |
| FastSerializer.dumps (small) | 17–88 µs | Section 4 |

---

## 优化条目总览（ROI 降序）

| ID | 类别 | 标题 | 收益 | 努力 | 风险 | ROI |
|----|------|------|------|------|------|-----|
| OPT-1 | Python | ColumnAccessor O(1) 字段查找 ✅ | ★★ | S | 极低 | ▶ 中（实际收益被 SWIG 掩盖） |
| OPT-1.1 | Python | ColumnAccessor numpy 数组缓存 ✅ | ★★★★★ | S | 低 | 🔥 极高 |
| OPT-1.2 | Python | ColumnAccessor 热路径单步查找 ✅ | ★★ | S | 极低 | ▶ 中（333→250 ns，__getattr__ 协议成瓶颈） |
| OPT-3 | Python | `__getattr__` if-chain → dict dispatch ✅ | ★★★ | S | 极低 | 🔥 极高（已完成） |
| OPT-2 | Python | `Feature._cache` 懒分配 | ★★ | S | 低 | ⬆ 高 |
| OPT-4 | API | `Table.iter_reuse()` — 复用 Feature 实例 ✅ | ★★★ | M | 低 | ⬆ 高 |
| OPT-5 | API | `Table.fill()` — 批量列写入 ✅ | ★★★★ | M | 中 | ⬆ 高 |
| OPT-6 | Python | 统一 ClassSchema cache ✅ | ★★ | L | 中 | ▶ 中 |
| OPT-7 | C++ | 批量 get_fields SWIG API | ★★★★ | L | 高 | ▶ 中 |
| OPT-8 | Bug | FastSerializer SIGBUS 修复 | ★★★★★ | XL | 极高 | ⚠ 必修 |

---

## 详细条目

---

### OPT-1 ｜ ColumnAccessor O(1) 字段查找

**类别**：Python 层 &nbsp;｜&nbsp; **努力**：S（~10 行）&nbsp;｜&nbsp; **风险**：极低

#### 问题

`table.column.x` 每次触发 `ColumnAccessor.__getattr__('x')`，里面调用 `get_all_defns(feature_type)` 拿到有序列表后做 **O(n) 线性扫描**（[table.py:50–56](../python/fastdb4py/orm/table.py)）。

实测 n=3 时已高达 **~9 µs**，且与 n=10 几乎无差别（固定开销占绝大部分）——说明 `get_all_defns()` 本身调用链（WeakKeyDict + sort）就花了大部分时间，而非扫描本身。

```
benchmark o0:  column_accessor_scan (last of 3)  → 8.67 µs
benchmark o0:  column_accessor_scan (last of 10) → 9.54 µs
```

column-wise read 比 row-wise 快 131×，但 **每次 `table.column.x` 访问本身就要 9 µs**，严重稀释了 numpy 零拷贝的优势。

#### 根因

`_create_column_accessor()` 动态创建的 `ColumnAccessor` 类没有在构造时绑定字段→index 映射，导致每次属性访问都重新查一遍。

#### 方案

在 `_create_column_accessor()` 生成类时，在闭包里**提前**计算 `field_name → column_index` 映射，注入给 `__init__`：

```python
# python/fastdb4py/orm/table.py   _create_column_accessor() 中

defns = get_all_defns(feature_type)
_field_index_map = {name: idx for idx, (name, _) in enumerate(defns)}

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

#### 文件改动

| 文件 | 位置 | 改动说明 |
|------|------|----------|
| `python/fastdb4py/orm/table.py` | `_create_column_accessor()` L14–72 | 新增闭包变量 + 重写 `__getattr__` |

#### 预期收益

| 指标 | 基线 | 目标 | 改善 |
|------|------|------|------|
| `column_accessor_scan` (3f) | 8.67 µs | ~300 ns | **~29×** |
| `column_accessor_scan` (10f) | 9.54 µs | ~300 ns | **~32×** |
| `point_cloud_read_columnwise` (N=1000) | 33.58 µs | ~10 µs | **~3×** |

> 预期值 ~300 ns = 1× WeakKeyDict class cache lookup + 1× dict.get + 1× SWIG get_column + 1× as_nparray

**benchmark 验证指标**：`column_accessor_scan`，`point_cloud_read_columnwise`

> **o1 实测更正**：OPT-1 实际只改善了 12–19%（而非预期 29×）。真实瓶颈是 SWIG `get_column(idx)` 调用本身（~4–5 µs/次），与字段名查找方式无关。见 OPT-1.1。

---

### OPT-1.1 ｜ ColumnAccessor numpy 数组缓存

**类别**：Python 层 &nbsp;｜&nbsp; **努力**：S（~10 行）&nbsp;｜&nbsp; **风险**：低
**依赖**：OPT-1 已完成 ✅

#### 问题

o1 实测发现 `table.column.x` 的 ~7.6 µs 开销来源：

| 步骤 | 估算耗时 |
|------|----------|
| `object.__getattribute__` ×2 | ~200 ns |
| `dict.get(name)` | ~50 ns |
| SWIG `get_column(idx)` | **~4–5 µs** ← 真实瓶颈 |
| `as_nparray()` + `__array_interface__` 包装 | ~500 ns–1 µs |

`ColumnAccessor` 实例在 `table.column` 属性中**已被缓存**（`table._column`），每次 `table.column` 返回的是同一个 `ColumnAccessor` 实例。因此可以在实例内部缓存每列的 numpy array，避免重复调用 `get_column()`。

由于 numpy array 是 C++ 内存的零拷贝 view（通过 `__array_interface__`），只要底层 `WxLayerTable` 不重新分配（fixed-scale table 不会），缓存是安全的。

#### 方案

在 `ColumnAccessor.__getattr__` 中首次访问时将 numpy array 存入实例级缓存字典，后续直接返回缓存值：

```python
class ColumnAccessor:
    def __init__(self, table_origin, feature_type):
        object.__setattr__(self, '_table_origin', table_origin)
        object.__setattr__(self, '_field_index_map', _field_index_map)
        object.__setattr__(self, '_array_cache', {})   # 新增：列数组缓存

    def __getattr__(self, name: str) -> np.ndarray:
        fmap = object.__getattribute__(self, '_field_index_map')
        idx = fmap.get(name)
        if idx is None:
            raise AttributeError(f'Field "{name}" not found in the table.')

        # O(1)：缓存命中时直接返回，跳过 SWIG get_column()
        cache = object.__getattribute__(self, '_array_cache')
        arr = cache.get(idx)
        if arr is not None:
            return arr

        table_origin = object.__getattribute__(self, '_table_origin')
        arr = table_origin.get_column(idx).as_nparray()
        cache[idx] = arr
        return arr
```

> **安全前提**：fixed-scale `WxLayerTable` 的列数据指针在整个生命周期内不移动。如果未来支持可变 table，需在写操作后调用 `_invalidate_array_cache()`。

#### 文件改动

| 文件 | 位置 | 改动说明 |
|------|------|----------|
| `python/fastdb4py/orm/table.py` | `_create_column_accessor()` ColumnAccessor 类 | `__init__` 加 `_array_cache`，`__getattr__` 加缓存命中路径 |

#### 预期收益

| 指标 | o1 实测 | 目标（首次访问） | 目标（缓存命中） |
|------|---------|-----------------|-----------------|
| `column_accessor_scan` (3f) | 7.62 µs | 7.6 µs（不变） | **~300 ns** |
| `column_accessor_scan` (10f) | 7.75 µs | 7.7 µs（不变） | **~300 ns** |
| `point_cloud_read_columnwise` (N=1000) | 33.58 µs | **~5 µs** | **~5 µs** |

> benchmark 的 `column_accessor_scan` 每次测量都会 `_create_column_accessor()` 一个新实例（因为 scan 取的是第 3 次 / 第 10 次访问同一个 accessor 实例），缓存命中效果会在 `point_cloud_read_columnwise` 中体现更明显（同一 `table.column.x` 反复访问）。

**benchmark 验证指标**：`column_accessor_scan`（缓存命中路径），`point_cloud_read_columnwise`

---

### OPT-1.2 ｜ ColumnAccessor 热路径单步查找

**类别**：Python 层 &nbsp;｜&nbsp; **努力**：S（~5 行改动）&nbsp;｜&nbsp; **风险**：极低
**依赖**：OPT-1.1 已完成 ✅

#### 问题

o1.1 热路径（缓存命中）仍有 2 次 `object.__getattribute__` + 2 次 `dict.get`（~290 ns），因为先查 `_field_index_map[name]→idx`，再查 `_array_cache[idx]→arr`，命中时 idx 完全是无用的中间层。

#### 方案

将 `_array_cache`（键为 idx）改为 `_name_cache`（键为 name），提到最前面。热路径：1× `object.__getattribute__` + 1× `dict.get`；冷路径才访问 `_field_index_map`。

```python
# __getattr__ 热/冷路径分离
name_cache = object.__getattribute__(self, '_name_cache')
arr = name_cache.get(name)
if arr is not None:
    return arr                       # 热路径：~160 ns

fmap = object.__getattribute__(self, '_field_index_map')
idx = fmap.get(name)                 # 冷路径
if idx is None: raise AttributeError(...)
arr = table_origin.get_column(idx).as_nparray()
name_cache[name] = arr
return arr
```

#### 预期 vs 实测

| 指标 | o1.1 | 预期 | 实测 | 说明 |
|------|------|------|------|------|
| `column_accessor_scan` | 333 ns | ~170 ns | **250 ns** | `__getattr__` 协议本身 ~80 ns 不可消除 |
| `point_cloud_read_columnwise` | 5.75 µs | ~3–4 µs | **5.58 µs** | 改善在误差范围内 |
| Column vs row-wise | 692× | — | **727×** | — |

> 250 ns 已接近 `__getattr__` 协议极限。进一步优化需改变 API 语义（绕过 `__getattr__`），ROI 递减。

**benchmark 验证**：[o1.2/benchmark.md](o1.2/benchmark.md)

---

### OPT-3 ｜ `__getattr__` if-chain → dict dispatch

**类别**：Python 层 &nbsp;｜&nbsp; **努力**：S（~15 行）&nbsp;｜&nbsp; **风险**：极低

#### 问题

db-mapped scalar read 的 `__getattr__` 在确定字段类型后，用最多 **8 个 if-elif 判断**路由到对应 SWIG getter（[feature.py:114–130](../python/fastdb4py/feature/feature.py)）。最常见的 F64 需要经过 4 个不匹配的分支才到达。

```
benchmark o0:  scalar_read_db_mapped (F64)  → 667 ns
               scalar_read_pure_python      → 167 ns   (4× 差距)
```

Python 解释器对每个 if 估计 5–15 ns，但分支预测频繁失中时代价更高。

#### 方案

模块级 dispatch dict，将 `OriginFieldType` 枚举值直接映射到 setter/getter 函数：

```python
# python/fastdb4py/feature/feature.py  模块顶层

_SCALAR_GETTER: dict[OriginFieldType, Callable] = {
    OriginFieldType.f64:  lambda o, fid: o.get_field_as_float(fid),
    OriginFieldType.f32:  lambda o, fid: o.get_field_as_float(fid),
    OriginFieldType.u32:  lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.i32:  lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.u8:   lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.u16:  lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.str:  lambda o, fid: o.get_field_as_string(fid),
    OriginFieldType.wstr: lambda o, fid: o.get_field_as_wstring(fid),
}

# __getattr__ 中替换 if-chain：
getter = _SCALAR_GETTER.get(ft)
if getter is not None:
    return getter(self._origin, fid)
```

`__setattr__` 同理，将数值类型集合改为 `frozenset` 做 O(1) `in` 判断。

#### 文件改动

| 文件 | 位置 | 改动说明 |
|------|------|----------|
| `python/fastdb4py/feature/feature.py` | L114–130（getter），L155–165（setter） | 替换 if-chain 为 dict dispatch |

#### 预期收益

| 指标 | 基线 | 目标 | 改善 |
|------|------|------|------|
| `scalar_read_db_mapped` (F64) | 667 ns | ~500 ns | ~25% |
| `scalar_write_db_mapped` | 1.29 µs | ~1.05 µs | ~20% |

> 注：SWIG 调用本身约 400 ns 不变，节省的是 Python if-chain 部分（~150–200 ns）

**benchmark 验证指标**：`scalar_read_db_mapped`，`scalar_write_db_mapped`

---

### OPT-2 ｜ `Feature._cache` 懒分配

**类别**：Python 层 &nbsp;｜&nbsp; **努力**：S（~20 行）&nbsp;｜&nbsp; **风险**：低

#### 问题

`Feature.__init__` 第一行分配 `self._cache: dict = {}`（[feature.py:18](../python/fastdb4py/feature/feature.py)），无论该对象是否需要缓存。

对于 db-mapped Feature（`table[i]`、`iter_table`、`map_from` 返回的对象），**_cache 永远不会被写入 scalar 值**——scalar 字段直接走 SWIG，只有 ref 字段首次访问后才会写缓存。大量迭代循环时，每帧都分配并很快丢弃一个空 dict，给 GC 造成不必要的压力。

```
benchmark o0:  feature_init_db_mapped → 2.50 µs
               iter_table per item    → 2.02 µs  (每步 map_from 一次)
```

#### 方案

将 `_cache` 初始化为 `None`，首次写入时才分配：

```python
# feature.py

def __init__(self, **kwargs):
    self._cache: dict | None = None   # 懒分配
    ...
    if kwargs:
        cache = {}
        for key, value in kwargs.items():
            if key.startswith('_'):
                object.__setattr__(self, key, value)
            else:
                cache[key] = value
        object.__setattr__(self, '_cache', cache)

# __getattr__：将 `if name in self._cache` 改为：
cache = self._cache
if cache is not None and name in cache:
    return cache[name]

# __setattr__ 写缓存处：
cache = self._cache
if cache is None:
    cache = {}
    object.__setattr__(self, '_cache', cache)
cache[name] = value
```

#### 文件改动

| 文件 | 位置 | 改动说明 |
|------|------|----------|
| `python/fastdb4py/feature/feature.py` | L16–36（init），L57–58（getattr），L144–145（setattr），L79–83（default ref） | 懒分配，所有 `_cache` 访问加 None 判断 |

#### 预期收益

| 指标 | 基线 | 目标 | 改善 |
|------|------|------|------|
| `feature_init_db_mapped` | 2.50 µs | ~2.20 µs | ~12% |
| `iter_table` per item | 2.02 µs | ~1.70 µs | ~16% |

> 主要收益体现在大量迭代时 GC 停顿减少，峰值内存下降

**benchmark 验证指标**：`feature_init_db_mapped`，`iter_table`

---

### OPT-4 ｜ `Table.iter_reuse()` — 复用 Feature 实例

**类别**：新增 API &nbsp;｜&nbsp; **努力**：M（+15 行，新方法）&nbsp;｜&nbsp; **风险**：低

#### 问题

`Table.__iter__` 每步创建新 Feature：`tryGetFeature(i)` + `map_from()`，分配 Feature 对象 + 5 个属性赋值（[table.py:96–98](../python/fastdb4py/orm/table.py)）。

```
benchmark o0:  iter_table (N=500) → 1.01 ms → 2.02 µs/item
               feature_init_db_mapped       → 2.50 µs
```

迭代 1000 个 feature = 2ms 纯分配+GC，实际 C++ 数据读取可以快 100×。

#### 方案

新增 `Table.iter_reuse()` 方法，内部复用同一 Feature wrapper，每步仅更新 `_origin` 指针：

```python
# python/fastdb4py/orm/table.py

def iter_reuse(self) -> Generator[T, None, None]:
    """
    高性能迭代器，复用同一 Feature 实例。
    警告：不能在迭代步之间保存引用，每步调用后 feature 对象被重置。
    """
    if not self.fixed:
        raise RuntimeError('iter_reuse only supports fixed-scale tables.')
    wrapper = self._feature_type()
    object.__setattr__(wrapper, '_db', self._db)
    count = self._origin.get_feature_count()
    for i in range(count):
        object.__setattr__(wrapper, '_origin', self._origin.tryGetFeature(i))
        object.__setattr__(wrapper, '_cache', None)
        yield wrapper
```

#### 文件改动

| 文件 | 位置 | 改动说明 |
|------|------|----------|
| `python/fastdb4py/orm/table.py` | `class Table` 末尾 | 新增 `iter_reuse()` 方法，不改现有接口 |

#### 预期收益

| 指标 | 基线 | 目标 | 改善 |
|------|------|------|------|
| `iter_table` per item | 2.02 µs | ~0.7 µs | **~3×** |

> 节省：Feature.__init__ + dict 分配 + GC；保留：tryGetFeature SWIG 调用（约 500 ns）

**benchmark 验证指标**：需在 benchmark 中新增 `iter_table_reuse` 对比用例

---

### OPT-5 ｜ `ORM.push_many()` — 批量写入

**类别**：新增 API &nbsp;｜&nbsp; **努力**：M（+30 行）&nbsp;｜&nbsp; **风险**：中

#### 问题

`push(feature)` 对每个 feature 执行：`get_all_defns()` + `add_feature_begin()` + N×`set_field()` + `add_feature_end()`，均摊 **~7.5 µs/feature**。SWIG 调用数 = 3 scalar fields × 1 call/field + 2 (begin/end) = 5 calls/feature。

```
benchmark o0:  ORM.create + push N=100  → 749 µs  (7.49 µs/feature)
               ORM.create + push N=500  → 3.76 ms  (7.52 µs/feature)
```

对于 truncate 路径（知道总量），可以完全绕过 push，直接用 `table.column.x[:] = array` 写入所有列：

```python
# 目标用法
db = ORM.truncate([TableDefn(Point, N)])
table = db[Point][Point]
xs = np.linspace(0, 1, N)
ys = np.zeros(N)
table.column.x[:] = xs   # 零拷贝，O(1) SWIG + O(N) memcpy
table.column.y[:] = ys
```

这条路径已完全可用，但 `push()` 接口没有引导用户走这条路。

**方案 A（文档 + 示例）**：在 `ORM.truncate` 返回后建议用 `table.column.field[:] = array` 批量写入，代价极低。

**方案 B（新 API）**：在 `ORM` 上新增 `push_from_arrays(feature_type, **col_arrays)` 方便接口：

```python
# python/fastdb4py/orm/__init__.py

def push_from_arrays(self, feature_type: Type[T], **col_arrays: np.ndarray):
    """
    从 numpy arrays 批量写入 truncate 模式的 table。
    要求 table 已通过 ORM.truncate 创建。
    col_arrays: field_name → np.ndarray，长度必须等于 table 容量。
    """
    table = self[feature_type][feature_type]
    for field_name, arr in col_arrays.items():
        col = getattr(table.column, field_name)
        col[:] = arr
```

#### 文件改动

| 文件 | 位置 | 改动说明 |
|------|------|----------|
| `python/fastdb4py/orm/__init__.py` | `class ORM` | 新增 `push_from_arrays()` 方法（约 15 行） |

#### 预期收益

| 场景 | 当前 | 目标 | 改善 |
|------|------|------|------|
| 写 N=1000 features（已 truncate） | ~7.5 ms | ~0.05 ms | **~150×** |
| 写 N=1000 features（dynamic push） | ~7.5 ms | 不变 | — |

**benchmark 验证指标**：新增 `point_cloud_write_from_arrays` 用例对比 `point_cloud_write_rowwise`

---

### OPT-6 ｜ 统一 ClassSchema cache

**类别**：Python 重构 &nbsp;｜&nbsp; **努力**：L（多文件，需全量测试）&nbsp;｜&nbsp; **风险**：中

#### 问题

目前有 4 个独立的 `WeakKeyDictionary` cache，各自持有一把 `Lock`：

| cache | 位置 | 存储内容 |
|-------|------|----------|
| `_feature_hints_cache` | `feature.py:12–13` | `get_type_hints()` 结果 |
| `_global_feature_defn_cache` | `utils.py:9` | `parse_defns()` 结果 |
| `_column_accessor_cache` | `table.py:11–12` | 动态 ColumnAccessor 类 |
| `_CLASS_SCHEMA_CACHE` | `serializer.py:12–13` | 序列化 schema |

每次一个新 Feature class 首次出现，会触发 **2–3 次独立的 `get_type_hints()` 调用**（feature init、parse_defns、serializer各一次），以及多次 Lock 竞争。

#### 方案

新增 `python/fastdb4py/feature/_schema.py`，定义统一的 `ClassSchema` 及全局缓存：

```python
@dataclass(frozen=True)
class ClassSchema:
    hints: dict                 # get_type_hints() 一次性结果
    defns: dict                 # name → (OriginFieldType, field_idx)
    ordered_defns: list         # [(name, OriginFieldType), ...] 按 idx 排序
    field_index_map: dict       # name → column_index（ColumnAccessor 用）
    serializer_schema: dict     # FastSerializer 专用的字段分类信息

_SCHEMA_CACHE: WeakKeyDictionary = WeakKeyDictionary()
_SCHEMA_LOCK: Lock = Lock()

def get_class_schema(cls) -> ClassSchema: ...
```

然后 `feature.py`、`utils.py`、`table.py`、`serializer.py` 全部改为调用 `get_class_schema(cls)` 取各自所需字段。

#### 文件改动

| 文件 | 位置 | 改动说明 |
|------|------|----------|
| `python/fastdb4py/feature/_schema.py` | 新文件 | ClassSchema 定义 + 统一 cache |
| `python/fastdb4py/feature/feature.py` | 全文 | 去掉 `_feature_hints_cache`，改用 `get_class_schema` |
| `python/fastdb4py/feature/utils.py` | 全文 | 去掉 `_global_feature_defn_cache` |
| `python/fastdb4py/orm/table.py` | `_create_column_accessor` | 去掉 `_column_accessor_cache` |
| `python/fastdb4py/serializer.py` | `_get_class_schema` | 去掉独立 cache |

#### 预期收益

| 指标 | 基线 | 目标 | 改善 |
|------|------|------|------|
| `schema_cache_miss` | 7.25 µs | ~3 µs | ~2× |
| `feature_init_pure_python` | 1.79 µs | ~1.3 µs | ~27% |
| 代码可维护性 | — | 显著提升 | 架构收益 |

**benchmark 验证指标**：`schema_cache_miss`，`feature_init_pure_python`，`feature_init_db_mapped`

---

### OPT-7 ｜ 批量 get_fields SWIG API

**类别**：C++/SWIG &nbsp;｜&nbsp; **努力**：L（需改 C++ + SWIG + 重新编译）&nbsp;｜&nbsp; **风险**：高

#### 问题

scalar read 667 ns 的构成估算：

| 部分 | 估算 |
|------|------|
| Python `__getattr__` + dict lookup | ~100–150 ns |
| SWIG 边界 （Python→C++ context） | ~350–400 ns |
| C++ 内存读取 | ~5–10 ns |

**SWIG 边界本身占 ~60%**，与读取多少个字段无关。当前 3 字段 Feature 需要 3 次 SWIG 调用（3×667 ns = 2 µs），而实际 C++ 侧只需要 3×5 ns = 15 ns。

#### 方案

在 SWIG interface 中新增批量读取方法，一次调用返回所有 scalar 字段到 ctypes 结构体或 numpy array：

```c
// fastcarto/fastdb/include/fastdb.h  新增
class FastVectorDbFeature {
    ...
    // 一次性读取指定 fields 的 double 值
    void get_fields_as_double(const int* field_ids, int count, double* out_buf) const;
    // 一次性写入
    void set_fields_from_double(const int* field_ids, int count, const double* values);
};
```

```python
# feature.py  使用 ctypes array 批量调用
# 代替：pt.x = ...; pt.y = ...; pt.z = ...
# 改为：_set_fields_batch(origin, [fid_x, fid_y, fid_z], [x, y, z])
```

#### 文件改动

| 文件 | 位置 | 改动说明 |
|------|------|----------|
| `fastcarto/fastdb/include/fastdb.h` | `FastVectorDbFeature` | 新增批量 get/set 声明 |
| `fastcarto/fastdb/src/fastdb*.cpp` | 对应 pimpl 实现 | 实现批量读写 |
| `fastcarto/fastdb/swig/fastdb4py.i` | `%extend FastVectorDbFeature` | SWIG 暴露批量接口 |
| `python/fastdb4py/feature/feature.py` | `__setattr__` 路径 | 使用批量 API |

需要重新运行 `uv pip install -e .` 编译。

#### 预期收益

| 指标 | 基线 | 目标 | 改善 |
|------|------|------|------|
| 3字段 Feature 完整 read | ~2 µs | ~700 ns | **~3×** |
| `point_cloud_read_rowwise` | 4.4 µs/pt | ~1.5 µs/pt | **~3×** |

**benchmark 验证指标**：`scalar_read_db_mapped`，`point_cloud_read_rowwise`，`ref_graph_traverse`

---

### OPT-8 ｜ FastSerializer SIGBUS 修复

**类别**：Bug Fix（C++）&nbsp;｜&nbsp; **努力**：XL &nbsp;｜&nbsp; **风险**：极高（内存 bug，需精确定位）

#### 问题

`FastSerializer.dumps()` 在 `List[Feature]` 元素数 ≥32 时进程以 exit code 138（SIGBUS）崩溃，小规模（N≤16）正常。

```python
pts = [BenchPoint(...) for i in range(32)]
FastSerializer.dumps(BenchPointCloud(points=pts))  # → SIGBUS
```

SIGBUS 通常意味着非对齐内存访问或对已释放/越界内存写入，是 C++ 层的 bug。

#### 定位步骤

1. **精确找崩溃阈值**：二分 N=16→32，找最小崩溃 N
2. **启用 AddressSanitizer 编译**：
   ```bash
   CFLAGS="-fsanitize=address" uv pip install -e .
   ```
   运行后 ASan 会给出精确的内存错误位置
3. **重点检查**：
   - `MemoryStream` 扩容逻辑：`realloc` 后是否更新了所有持有 `data_ptr` 的间接指针
   - `WxLayerTableBuild` 在 `add_feature_begin()` 时的内存预分配策略
   - blob 写入（`set_geometry_raw`）时的缓冲区边界

4. **相关 C++ 文件**：`fastcarto/fastdb/src/` 下的 `MemoryStream` 及 `FastVectorDbLayerBuild` 实现

#### 预期收益

| 指标 | 当前 | 修复后 |
|------|------|--------|
| `List[Feature]` N=32 | 崩溃 | 正常运行 |
| benchmark point_cloud | 限制在 N≤16 | 可测试 N=32,64,256 |

---

## 实施路线图

```
round o1  ─── OPT-1 + OPT-3          (同文件，同时做，纯 Python，极低风险)
              预期：column_accessor_scan ~9µs → ~300ns
                    scalar_read        667ns  → ~500ns

round o2  ─── OPT-2                  (Feature._cache 懒分配)
              预期：iter_table 2µs/item → ~1.7µs/item

round o3  ─── OPT-4                  (Table.iter_reuse 新方法)
              预期：新 API 下 iter 2µs/item → ~0.7µs/item

round o4  ─── OPT-5                  (push_from_arrays 新方法)
              预期：truncate 写场景 7.5ms → ~0.05ms (150×)

round o5  ─── OPT-6                  (统一 ClassSchema，大重构，需完整回归)
              预期：schema miss 7µs → ~3µs，代码架构改善

round o6  ─── OPT-7                  (C++ 批量 get_fields，需重编译)
              预期：scalar read ~3× 加速

round o7  ─── OPT-8                  (SIGBUS bug 修复，需 ASan 定位)
              预期：List[Feature] 大规模不再崩溃
```

---

## 决策矩阵（一图速查）

```
收益
  │
高 │  OPT-5(push_many)    OPT-7(C++批量)
  │       OPT-1(col O(1))             OPT-8(SIGBUS)
  │  OPT-4(iter_reuse)
  │
中 │        OPT-3(dispatch)
  │     OPT-6(schema)
  │
低 │    OPT-2(lazy cache)
  │
  └──────────────────────────────────────────────
     S              M              L           XL
                  改动成本 →
```

**建议起点**：OPT-1 + OPT-3（左上角，成本最低、收益高、零风险，一个 PR 即可完成）

---

## 每轮 benchmark 追踪表

| 优化轮次 | 完成 OPT | 关键指标对比文件 |
|----------|----------|-----------------|
| o0 | — 基线 | [o0/benchmark.md](o0/benchmark.md) |
| o1 | OPT-1 + OPT-3 ✅ | [o1/benchmark.md](o1/benchmark.md)（scalar_read ↑19%，row-wise ↑10–12%；column_scan 真实瓶颈为 SWIG get_column ~5µs） |
| o1.1 | OPT-1.1 ✅ | [o1.1/benchmark.md](o1.1/benchmark.md)（column_scan 8.67µs→333ns **26×**；column_read 33µs→5.75µs **5.8×**；Column vs row = **692×**） |
| o1.2 | OPT-1.2 ✅ | [o1.2/benchmark.md](o1.2/benchmark.md)（column_scan 333ns→**250ns 1.33×**；总计 o0→**35×**；`__getattr__` 协议成新下限 ~80ns） |
| o2 | OPT-2 ✅ | [o2/benchmark.md](o2/benchmark.md)（iter_table ↑4%；dict free-list 使 alloc 仅 ~80 ns，真瓶颈是 WeakKeyDict×2 → OPT-6） |
| o3 | OPT-4 ✅ | [o3/benchmark.md](o3/benchmark.md)（iter_reuse N=500: 976µs→**232µs 4.2×**；0.46µs/item vs 1.95µs/item；超出预期 2.8×） |
| o4 | OPT-5 ✅ | [o4/benchmark.md](o4/benchmark.md)（fill 3-col N=1000: **2.75 µs vs rowwise 6.04ms → ~2200×**；API 更简洁） |
| o5 | OPT-6 ✅ | [o5/benchmark.md](o5/benchmark.md)（feature_init_pure_python 1.83µs→**1.25µs −32%**；feature_init_db_mapped 2.42µs→**1.71µs −29%**；schema_cache_miss 7.29µs→6.17µs −15%；4 WeakKeyDict→1 ClassSchema） |
| o6 | OPT-7 | o6/benchmark.md（待创建） |
| o7 | OPT-8 | o7/benchmark.md（待创建） |
