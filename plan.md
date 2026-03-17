# fastdb4py 性能优化计划

> Benchmark 结果见 [benchmark.md](benchmark.md)

---

## 真实性能基线（macOS, Apple Silicon, Python 3.13.3）

| 操作 | 实测延迟 |
|------|----------|
| Pure Python scalar read | 167 ns |
| DB-mapped scalar read (SWIG) | 667 ns |
| DB-mapped scalar write (SWIG) | 1.29 µs |
| Ref resolve (首次) | 5.04 µs |
| ColumnAccessor 字段扫描 | ~9 µs（固定，与字段数关系弱） |
| Point cloud row-wise read (N=1000) | 4.40 ms → 4.4 µs/pt |
| Point cloud column-wise read (N=1000) | 33.6 µs → **33 ns/pt**（131× 快） |
| ORM.push per feature (3× F64) | ~7.5 µs |
| FastSerializer.dumps (small) | 17–88 µs |

---

## 优化策略（按可行性排序）

### 优先级 1：高收益、低风险、纯 Python 层改动

---

#### OPT-1：ColumnAccessor 字段名 → index 预计算缓存

**问题**：`ColumnAccessor.__getattr__('x')` 每次都调用 `get_all_defns()` 返回有序列表，然后 O(n) 线性扫描找字段 index（`table.py:50–56`）。即使 n=3，也要 **~9 µs**，远超后续 numpy 操作本身。

**根因**：`get_all_defns()` 返回 `list[tuple[str, OriginFieldType]]`，必须线性扫描才能找到 index。

**方案**：在 `_create_column_accessor()` 时，一次性构建 `field_name → column_index` 的 `dict`，作为 `ColumnAccessor` 实例属性存储。`__getattr__` 改为 O(1) dict 查找。

```python
# table.py _create_column_accessor 中
defns = get_all_defns(feature_type)
field_index_map = {name: idx for idx, (name, _) in enumerate(defns)}

class ColumnAccessor:
    def __init__(self, table_origin, feature_type):
        object.__setattr__(self, '_table_origin', table_origin)
        object.__setattr__(self, '_field_index_map', field_index_map)  # 预计算

    def __getattr__(self, name):
        idx = object.__getattribute__(self, '_field_index_map').get(name)
        if idx is None:
            raise AttributeError(f'Field "{name}" not found.')
        table_origin = object.__getattribute__(self, '_table_origin')
        return table_origin.get_column(idx).as_nparray()
```

**改动范围**：仅 `python/fastdb4py/orm/table.py`，~10 行
**预期收益**：`table.column.x` 从 ~9 µs → ~500 ns（约 **18× 加速**，去掉 O(n) 扫描）
**风险**：极低，只改内部实现，API 不变

---

#### OPT-2：`Feature._cache` 懒分配

**问题**：每个 `Feature.__init__` 都分配一个空 `dict` 作为 `_cache`（`feature.py:18`），即使对于 db-mapped 的只读 Feature（`table[i]` 返回的），这个 dict 永远不会被写入。大量 `for pt in table` 循环时，每次 `map_from` 都分配并立即丢弃一个空 dict。

**方案**：将 `_cache` 改为 `None`，仅在首次写入时分配：

```python
# feature.py
def __init__(self, **kwargs):
    self._cache = None  # 懒分配
    ...

def _get_cache(self):
    if self._cache is None:
        object.__setattr__(self, '_cache', {})
    return self._cache
```

在 `__getattr__` 和 `__setattr__` 中用 `self._get_cache()` 替换直接的 `self._cache` 访问。

**改动范围**：`python/fastdb4py/feature/feature.py`，~20 行
**预期收益**：纯读场景（行遍历 + 仅读字段）内存分配减少，GC 压力降低；`iter_table` 2 µs/item 可降至 ~1.5 µs
**风险**：低，需仔细处理 `_cache is None` 的各个分支，保证写入时正确初始化

---

#### OPT-3：`__getattr__` if-chain → dict dispatch

**问题**：db-mapped scalar read 在 `__getattr__` 中执行最多 8 个 `if ft == OriginFieldType.xxx` 分支（`feature.py:114–130`），即使对于最常见的 F64 也要经过 4–5 个 if 判断。

**方案**：用 `_GETTER_DISPATCH: dict[OriginFieldType, Callable]` 替换 if-chain：

```python
_GETTER_DISPATCH = {
    OriginFieldType.f64:  lambda origin, fid: origin.get_field_as_float(fid),
    OriginFieldType.f32:  lambda origin, fid: origin.get_field_as_float(fid),
    OriginFieldType.u32:  lambda origin, fid: origin.get_field_as_int(fid),
    OriginFieldType.i32:  lambda origin, fid: origin.get_field_as_int(fid),
    OriginFieldType.u8:   lambda origin, fid: origin.get_field_as_int(fid),
    OriginFieldType.u16:  lambda origin, fid: origin.get_field_as_int(fid),
    OriginFieldType.str:  lambda origin, fid: origin.get_field_as_string(fid),
    OriginFieldType.wstr: lambda origin, fid: origin.get_field_as_wstring(fid),
}

# 在 __getattr__ 中：
getter = _GETTER_DISPATCH.get(ft)
if getter:
    return getter(self._origin, fid)
```

**改动范围**：`python/fastdb4py/feature/feature.py`，~15 行
**预期收益**：scalar read 从 667 ns 降至约 500 ns（省去 if-chain 扫描，约 **25% 改善**）
**风险**：极低，纯逻辑等价替换，lambda 调用比 if-chain 的预测分支友好

---

### 优先级 2：中等收益、中等风险

---

#### OPT-4：`Table.__iter__` / `__getitem__` 复用 Feature 实例

**问题**：`for pt in table` 每步都调用 `BenchPoint.map_from()` 创建新 Feature 对象（`table.py:98`），实测 2 µs/item，N=1000 就是 2ms 的纯分配开销。

**方案 A（简单）**：提供 `Table.iter_reuse()` 方法，内部维护一个可复用的 Feature 实例，每步只更新 `_origin` 指针：

```python
def iter_reuse(self) -> Generator[T, None, None]:
    """复用同一 Feature 包装器，每步更新 _origin。禁止跨步持有引用。"""
    wrapper = self._feature_type()   # 一次分配
    wrapper._db = self._db
    for i in range(self._origin.get_feature_count()):
        object.__setattr__(wrapper, '_origin', self._origin.tryGetFeature(i))
        object.__setattr__(wrapper, '_cache', None)   # 清空缓存
        yield wrapper
```

**改动范围**：`python/fastdb4py/orm/table.py`，+10 行（新增方法，不改现有接口）
**预期收益**：迭代场景从 2 µs/item → ~0.8 µs/item（消除 Feature 对象分配和 GC）
**风险**：中，用户不能在循环外持有 `pt` 引用（需文档说明），语义上类似 C++ iterator

---

#### OPT-5：`ORM.push` 批量写入接口

**问题**：`push(feature)` 对每个 feature 有 `get_all_defns()` 查找 + N 次 SWIG `set_field()` 调用，均摊 ~7.5 µs/feature。批量 push 1000 features = 7.5ms 纯 Python+SWIG overhead。

**方案**：提供 `push_many(features: list[Feature])` 或 `push_numpy(xs: np.ndarray, ys: np.ndarray, ...)` 接口，在 Python 层收集所有数据后一次性写入，减少 SWIG 调用次数。

对于 truncate 路径（已预分配），可以直接利用 `table.column.x[:] = xs_array` 这条已有的 numpy 路径完全绕过逐 feature push。

**改动范围**：`python/fastdb4py/orm/__init__.py`，+30 行
**预期收益**：批量写 N=1000 features 从 7.5ms → ~0.1ms（利用 numpy column 写路径，**75× 加速**）
**风险**：中，需要 truncate 预分配后使用，不适用于 dynamic push 场景

---

#### OPT-6：合并 4 个独立 cache 为统一 ClassSchema

**问题**：目前有 4 个独立的 `WeakKeyDictionary` cache（features hints、field defns、column accessor、serializer schema），每次 class 首次使用都触发多次 `get_type_hints()` 调用（理论上 2–3 次）。

**方案**：引入统一的 `_ClassSchema` dataclass，在 class 第一次使用时一次性计算所有派生数据，存入单个 `WeakKeyDictionary`：

```python
@dataclass
class _ClassSchema:
    hints: dict          # get_type_hints() 结果
    defns: dict          # parse_defns() 结果：name → (OriginFieldType, idx)
    ordered_defns: list  # get_all_defns() 结果：[(name, type), ...]
    field_index_map: dict # name → column index（给 ColumnAccessor 用）
```

**改动范围**：新文件 `python/fastdb4py/feature/_schema.py` + 修改 `feature.py`、`utils.py`、`table.py`、`serializer.py`
**预期收益**：class 初始化从 3× `get_type_hints()` → 1×，重构后代码更清晰
**风险**：中高，涉及多文件改动，需确保所有现有缓存行为等价

---

### 优先级 3：高收益但高风险（需 C++ 改动）

---

#### OPT-7：C++ 批量 get_fields API ✅（已实现，o7）

**问题**：scalar read 667 ns 中约 400 ns 是 SWIG 边界开销（Python→C++ context switch），实际 C++ 读内存只需 ~5 ns。每个字段一次 SWIG 调用，3 字段就是 3× 边界开销。

**已实现**：`getFieldsAsDoubles` / `setFieldsFromDoubles` C++ 批量方法 + SWIG `%extend` 块 + Python `read_all_scalars()` / `write_all_scalars()`。

**实测收益**（o7）：
- `feature_batch_read` 3×F64：**208 ns**（vs 3×417 ns = 1251 ns，**6.0× 加速**）
- `feature_batch_write` 3×F64：**208 ns**（vs 3×875 ns = 2625 ns，**12.6× 加速**）

**已知设计缺陷与精度分析**（详见下方"OPT-7 设计反思"）：

**改动范围**：`fastcarto/fastdb/include/fastdb.h` + `src/FastVectorDbLayer.cpp` + `swig/fastdb4py.i` + `_schema.py` + `feature.py`
**风险**：高，需 C++ 改动 + SWIG 重新生成 + 重新编译

---

#### OPT-7 设计反思：double 中转的三个问题

##### 问题一：内存膨胀

**结论**：无持久膨胀，但有临时分配问题。

double 数组是**调用期间的临时 transfer buffer**，不改变 C++ 侧的存储格式（u8 仍 1 字节，f32 仍 4 字节）。

实际隐患：`read_all_scalars(out=None)` 在循环中未传 `out` 时，每次调用分配一个新 numpy float64 数组（N_fields × 8 字节）。迭代 1000 行 = 1000 次小分配。`get_fields_into(fids, out)` 热路径设计用于避免此问题——调用者预分配一个 `out` 并复用。

##### 问题二：精度丢失

**写路径（`setFieldsFromDoubles`）：无精度损失**

`set_field_value_t<double>` 对每种类型做显式 cast（[FastVectorDbLayerBuild_p.h:132-193](fastcarto/fastdb/src/FastVectorDbLayerBuild_p.h#L132-L193)）：
- `ftU8/U16/U32/I32`：整型 cast，所有值 < 2^53，double 可精确表示
- `ftF32`：f64 → f32 有精度收窄，但与原有 `setField(double)` 行为完全一致，无额外回归
- `ftF64`：完全无损
- `ftU8n/U16n`：线性归一化后截断，对称于读取路径

**读路径（`getFieldsAsDoubles`）：存在实际 Bug ⚠️**

`getFieldAsFloat_internal` 的 switch 语句**只处理 ftF32 / ftF64 / ftU8n / ftU16n**，对 ftU8 / ftU16 / ftU32 / ftI32 返回 `NAN`（[FastVectorDbLayer.cpp:347-364](fastcarto/fastdb/src/FastVectorDbLayer.cpp#L347-L364)）：

```cpp
switch (fd->type) {
case ftF32:  return *(f32*)ptr;
case ftF64:  return *(f64*)ptr;
case ftU8n:  { return vmin + (vmax-vmin) * v / 255.0; }
case ftU16n: { return vmin + (vmax-vmin) * v / 65535.0; }
}
return NAN;   // ← u8/u16/u32/i32 全部走到这里！
```

`_SCALAR_ORIGIN_TYPES` 中包含了 u8/u16/u32/i32，因此对含整型字段的 Feature 调用 `read_all_scalars()` / `get_fields_into()` 会**静默返回 NAN**。

Benchmark 中 BenchPoint 使用的是 3×F64，因此未暴露此 Bug。

**预修复方向**：`getFieldsAsDoubles` 内部对整型字段改用 `getFieldAsInt_internal()` 读取并转为 double，或在 `getFieldAsFloat_internal` 中补齐整型 case：
```cpp
case ftU8:  return *(u8*)ptr;
case ftU16: return *(u16*)ptr;
case ftU32: return *(u32*)ptr;
case ftI32: return *(i32*)ptr;
```

##### 问题三：string 等非数值类型的区分

Python 层在 `_SCALAR_ORIGIN_TYPES` 中正确排除了 `str`、`wstr`、`ref`、`bytes`，`scalar_field_ids_np` 不包含这些字段的 index，批量 API 不会接触它们。

字符串字段仍通过原有的 `__getattr__` → `get_field_as_string()` / `get_field_as_wstring()` 路径访问，无变化。

注意：`set_field_value_t` 中存在对 `ftSTR/ftWSTR` 的处理分支（将 double 值强转为字符串表索引），这是 build 阶段的遗留代码，在正确使用时不会被 `setFieldsFromDoubles` 触达。

---

#### OPT-8：FastSerializer `List[Feature]` SIGBUS 修复

**问题**：`FastSerializer.dumps()` 在 `List[Feature]` 元素数 ≥32 时触发 SIGBUS（macOS exit 138），说明 C++ 层存在内存越界。

**定位方向（按优先级）**：
1. 检查 `MemoryStream` 的 `realloc` 逻辑：blob 数据增大时缓冲区扩容是否正确更新所有内部指针
2. 检查 `WxLayerTableBuild.add_feature_begin()` 的内存预分配是否与实际写入量匹配
3. 精确定位崩溃阈值：二分 N=16→32 找到最小崩溃 N

**改动范围**：`fastcarto/fastdb/src/` C++ 源码
**风险**：高，内存 bug 定位需 LLDB + AddressSanitizer

---

## 实施路线图

```
Phase 1（纯 Python，安全）
  OPT-1: ColumnAccessor O(1) 字段查找     ← 最高性价比
  OPT-2: Feature._cache 懒分配           ← 内存优化
  OPT-3: __getattr__ dict dispatch      ← 小幅提速

Phase 2（新增 API，向后兼容）
  OPT-4: Table.iter_reuse()             ← 迭代性能
  OPT-5: push_many() / numpy 写入       ← 批量场景

Phase 3（重构，需完整测试）
  OPT-6: 统一 ClassSchema cache         ← 架构清理

Phase 4（C++ 改动，需重新编译）
  OPT-7: 批量 get_fields SWIG API       ← 深层加速
  OPT-8: List[Feature] SIGBUS 修复      ← Bug 修复
```

---

## 已发现问题（待修复）

### Bug: ORM.share() 共享内存名称超长导致 OSError（macOS）

**现象**：`test_shared_memory.py` 在 macOS 上失败，报错 `OSError: [Errno 63] File name too long`。

**根因**：macOS POSIX 共享内存名称上限为 31 个字符（含前导 `/`），但 `ORM.share()` 使用的默认名称格式为 `fastdb_test_{uuid4().hex}`（共 43 字符 + `/` = 44 字符），超出限制。

**位置**：`python/fastdb4py/orm/__init__.py` `share()` 方法 + 测试中的 shm_name 生成逻辑。

**修复方向**：`_normalize_shm_name()` 中对名称长度进行截断或改用更短的命名策略（如 `fdb_{uuid[:8]}`）。

---

### Bug: FastSerializer 崩溃于 List[Feature] 元素数 ≥32

**现象**：`FastSerializer.dumps(BenchPointCloud(points=[...]))` 在 `points` 列表包含 ≥32 个 `BenchPoint` 时，进程以 exit code 138（SIGBUS）崩溃。

**复现**：
```python
pts = [BenchPoint(x=float(i), y=float(i)*0.5, z=0.0) for i in range(32)]
root = BenchPointCloud(points=pts)
FastSerializer.dumps(root)  # → SIGBUS, exit 138
```

**小规模（N=4, 8, 16）正常**，崩溃阈值在 16~32 之间（待精确定位）。

**初步分析**：SIGBUS 通常来自 C++ 非对齐内存访问或越界写入。怀疑 `FastSerializer` 在写入 `List[Feature]` 类型的 blob 时，大量对象导致 `WxMemoryStream` 缓冲区扩容或 `WxLayerTableBuild` 内部容量溢出。

**定位方向**：
- `serializer.py` 中 `List[Feature]` 的 blob 编码路径
- C++ `MemoryStream` 在大数据量时的扩容逻辑（`fastcarto/fastdb/src/`）
- `WxLayerTableBuild.add_feature_begin/end` 的内存边界

**临时规避**：Benchmark 中将 `List[Feature]` 测试规模限制在 N≤16。

---

## 参考文件

| 文件 | 关联优化 |
|------|----------|
| `python/fastdb4py/orm/table.py:47–58` | OPT-1 ColumnAccessor，OPT-4 iter_reuse |
| `python/fastdb4py/feature/feature.py:55–131` | OPT-2 懒分配，OPT-3 dict dispatch |
| `python/fastdb4py/feature/utils.py` | OPT-6 统一 schema cache |
| `python/fastdb4py/orm/__init__.py` | OPT-5 push_many |
| `python/fastdb4py/serializer.py` | OPT-8 SIGBUS |
| `fastcarto/fastdb/swig/fastdb4py.i` | OPT-7 批量 get_fields |
| `tests/python/benchmark_comprehensive.py` | 所有优化的验证工具 |
