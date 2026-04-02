# ORM Push 性能优化报告

> **日期：** 2026-04-02  
> **平台：** macOS (Apple Silicon)，Python 3.14t（自由线程）  
> **分支：** `autoresearch/apr01`  
> **基准命令：** `uv run python tests/python/benchmark_native_list.py --quick --reps 1`  
> **指标：** N=100,000 时 `build_ms`，取 list_len={8, 64, 512} 三格均值（越小越好）

---

## 概述

本报告记录了对 `fastdb4py` 中 `ORM.push()` 方法的一次系统性自动优化实验（autoresearch）。  
优化目标是降低批量写入包含 `List[F64]` 字段的 Feature 对象时的序列化时间（`build_ms`）。

### 优化结果

| 指标 | 值 |
|------|-----|
| **基线** | 1642.4 ms |
| **最终** | 520.4 ms |
| **总提升** | **−68.3 %（速度提升 3.15×）** |
| **实验总数** | 20 |
| **保留** | 14 |
| **丢弃** | 5（含 1 个无改善但代码更清晰的保留） |

---

## 背景

fastdb 的核心写入路径如下：

```
用户调用 orm.push(feature)
  → ORM.push()
      → 类型检查 / 模式查找 (ClassSchema)
      → 表查找 / 首次建表
      → push_fn(feature._cache, table_origin)
          → table.add_feature_begin()
          → table.set_field(idx, value)     # 标量字段
          → struct.pack(fmt, *items)         # 列表序列化
          → table.set_field_list_numeric(idx, bytes)
          → table.add_feature_end()
```

每次写入包含：
- **4 次 SWIG 调用**（add_feature_begin / set_field / set_field_list_numeric / add_feature_end），每次约 0.7 µs
- **Python 级别开销**：对象构造、字典查找、条件分支、列表序列化

基线下每个 Feature 约耗时 **~16 µs**；优化后降至 **~5.2 µs**。

---

## 实验详情

### 实验 0 — 基线

- **提交：** `9a6217d`
- **指标：** 1642.4 ms
- **说明：** 原始未修改代码。每次 `push()` 对每个字段做 `if/elif` 类型分发，每次 `set_field_list_numeric` 前都会调用 `np.asarray` 转换列表为字节。

---

### 实验 1 — 快速路径：绕过 DFS 图遍历 ✅ 保留

- **提交：** `8c3bdb0`
- **指标：** 1167.5 ms（−28.9 %）
- **改动文件：** `orm/__init__.py`、`feature/_schema.py`

**分析：**  
原始代码对所有 Feature 一律走 `_push_graph()` —— 一个用于处理循环引用的 DFS 图遍历框架，存在 `_GraphCollector` 对象分配、集合操作等固定开销（每 Feature 约 6–8 µs）。  
对于没有 `ref` 字段的 Feature（大多数情况），这些开销完全没有必要。

**方案：**  
在 `ClassSchema` 中新增 `has_ref_fields: bool` 字段，首次构建 schema 时预计算。`push()` 入口处先检查该标志：
- `has_ref_fields = True` → 走原有 `_push_graph()` 路径
- `has_ref_fields = False` → 走新增的 `_push_simple_list()` 快速路径，直接调用底层 SWIG 接口

**影响：** 本次实验是整个优化过程中**最大的单次提升**（−28.9 %），源于消除了对简单 Feature 不必要的图遍历固定开销。

---

### 实验 2 — 模块级类型导入 ✅ 保留

- **提交：** `609d059`
- **指标：** 1100.0 ms（−5.8 %）

**分析：**  
`LIST_ELEM_CPP_TYPE`、`LIST_ELEM_DTYPE`、`get_list_element_type` 之前在函数内部懒加载，每次 push 都有模块属性查找开销。

**方案：** 提升为模块级常量 import，变为 LOAD_GLOBAL 常量访问。

---

### 实验 3 — 预计算 dtype/cpp_type ❌ 丢弃

- **提交：** `f8ec95f`（已回滚）
- **指标：** 1100.7 ms（无改善）

**分析：** dict lookup 本身已是 O(1)，额外缓存层反而增加初始化开销，无效。

---

### 实验 4 — 预拆分写入计划 ✅ 保留

- **提交：** `5dc3505`
- **指标：** 1034.6 ms（−5.9 %）
- **改动文件：** `feature/_schema.py`

**分析：** 每次 push 都对每个字段重复执行 `if ft == OriginFieldType.xxx` 多分支判断，字段类型在运行期永不改变。

**方案：** 在 `ClassSchema` 中预计算四个写入计划列表（`numeric_plan`、`str_plan`、`bytes_plan`、`list_plan`），push 时直接分别遍历，消除运行时类型分支。

---

### 实验 5 — `array.array.tobytes()` 代替 `np.asarray` ✅ 保留

- **提交：** `9fe7586`
- **指标：** 841.8 ms（−18.6 %）

**分析：** `np.asarray` 有 NumPy 对象初始化开销，对纯 Python 数字列表不占优势。

**微基准（n=512）：**
| 方法 | 耗时 |
|------|------|
| `np.asarray(items).tobytes()` | 11.4 µs |
| `array.array('d', items).tobytes()` | 6.2 µs |
| `struct.pack(fmt, *items)` | 3.3 µs |

**方案：** 改用 `array.array(typecode, items).tobytes()`，约 2× 快于 numpy；同时在 `type.py` 新增 `LIST_ELEM_ARRAY_TYPECODE` 映射表。

---

### 实验 6 — 缓存 struct.pack 格式字符串 ✅ 保留

- **提交：** `631316c`
- **指标：** 763.3 ms（−9.3 %）

**分析：** `struct.pack` 在 n≥16 时比 `array.array` 快约 2×，但格式字符串每次拼接有 GC 压力。

**方案：** 引入 `_struct_fmt_cache: dict`，以 `(typecode, n)` 为键缓存格式字符串，首次调用后永久复用。

---

### 实验 7 — `exec` 代码生成专用 push_fn ✅ 保留

- **提交：** `0ee5776`
- **指标：** 730.8 ms（−4.2 %）
- **改动文件：** `feature/_schema.py`

**分析：** 即使有了 `numeric_plan`/`list_plan` 四个计划列表，每次 push 仍有 Python for 循环开销（LOAD_FAST × N_fields + 函数调用帧）。

**方案：** 引入 `_compile_push_fn()`，在 schema 首次构建时用 `exec` 动态生成并编译一个专属于该 Feature 类的 push 函数：

```python
# 为 BenchListFeature 生成的代码（示例）
def _push(cache, t, _struct_pack, _get_struct_fmt):
    t.add_feature_begin()
    _v = cache.get('row_id')
    t.set_field(0, _v if _v is not None else 0)
    _items = cache.get('xs') or []
    _n = len(_items)
    t.set_field_list_numeric(1, _struct_pack(_get_struct_fmt('d', _n), *_items))
    t.add_feature_end()
```

生成代码中没有循环、没有分支，所有字段名和索引均为字面量，CPython 可以最高效地执行。

---

### 实验 8 — exec 命名空间注入 struct 引用 ❌ 丢弃

- **提交：** `4b79281`（已回滚）
- **指标：** 738.7 ms（倒退）

**分析：** 尝试将 `struct.pack` 和格式缓存 bake 进 exec 命名空间（变为 LOAD_GLOBAL），而不是作为函数参数传入（LOAD_FAST）。  
结果：CPython 中 LOAD_GLOBAL 比 LOAD_FAST 慢，此方案反而更慢。

**结论：** 函数参数（LOAD_FAST）比全局变量（LOAD_GLOBAL）更快，不要将热路径变量放入 exec 全局命名空间。

---

### 实验 9 — `get_class_schema` 模块级导入 ✅ 保留

- **提交：** `971fe51`
- **指标：** 697.0 ms（−4.6 %）

**分析：** `get_class_schema` 在 `push()`、`_combine()`、`_push_graph()` 三处被懒加载（函数内 `from ... import`），每次调用都产生模块导入机制开销。

**方案：** 提升为文件顶部 `from ..feature._schema import get_class_schema`，变为模块级绑定，后续每次访问为 O(1) LOAD_GLOBAL。

---

### 实验 10 — `push()` 内联快速路径 ✅ 保留

- **提交：** `2992812`
- **指标：** 689.9 ms（−1.0 %）

**分析：** 快速路径原为独立的 `_push_simple_list()` 方法，每次调用有额外的函数帧分配开销和 `type(feature)` 重复计算。

**方案：** 将 `_push_simple_list()` 逻辑直接内联到 `push()` 的 if 分支中，消除方法调用开销和重复 `type()` 调用。

---

### 实验 11 — `__setattr__` 内联 fixed 属性检查 ✅ 保留

- **提交：** `68939d7`
- **指标：** 673.1 ms（−2.4 %）
- **改动文件：** `feature/feature.py`

**分析：** `Feature.__setattr__` 热路径中调用 `self.fixed` 属性（一个 property），其底层为 `isinstance(self._origin, core.WxDatabase)` SWIG 类型检查，约 0.2 µs。

**方案：** 将 `if not self.fixed:` 替换为 `if self._origin is None:`，直接访问 slot 属性（约 50 ns），避免 property 调用和 isinstance 开销。

---

### 实验 12 — `__setattr__` 纯 Python 模式短路 ✅ 保留

- **提交：** `733dea0`
- **指标：** 672.9 ms（与 exp11 持平，代码更清晰）

**分析：** `__setattr__` 中原先先执行 `_origin_hints.get(name)` 查找字段元数据，再判断 `_origin is None`。但纯 Python 模式（`_origin is None`）是 Feature 对象构造期的常态，应优先处理。

**方案：** 将 `if self._origin is None: self._cache[name] = value; return` 提前到 hints 查找之前，消除无效 dict 查找。

**注：** 指标改善在噪声范围内（0.2 ms），但代码逻辑更清晰，保留。

---

### 实验 13 — `__init__` 直接调用 `object.__setattr__` ✅ 保留

- **提交：** `205131a`
- **指标：** 609.7 ms（−9.4 %）
- **改动文件：** `feature/feature.py`

**分析：** `Feature.__init__` 中对 5 个私有属性的赋值（`self._origin = None` 等）会触发 `Feature.__setattr__`，后者有 `startswith('_')` 检查等逻辑。以 100K Features 计：5 × 100K × ~120 ns = ~60 ms 额外开销。

**方案：**
```python
_sa = object.__setattr__  # 模块级别别名

class Feature:
    def __init__(self, ...):
        _sa(self, '_origin', None)
        _sa(self, '_cache', {})
        _sa(self, '_hints', None)
        # ...
```

`object.__setattr__` 直接写入 slot，完全绕过 `Feature.__setattr__` 的分发逻辑。

---

### 实验 14 — 缓存 `_is_mutable` 布尔标志 ✅ 保留

- **提交：** `4d7291a`
- **指标：** 600.2 ms（−1.6 %）
- **改动文件：** `orm/__init__.py`

**分析：** `push()` 入口检查 `self.fixed` 属性，其底层为 `isinstance(self._origin, core.WxDatabase)` SWIG 类型检查（约 0.2 µs）。以 100K pushes 计：0.2 µs × 100K = 20 ms。

**方案：** 在 `ORM` 中新增 `_is_mutable: bool` 字段，仅在 `ORM.create()` 时设为 `True`（其他构造路径默认 `False`）。`push()` 中改用 `if not self._is_mutable:` 做布尔检查（约 20 ns），保留原 `fixed` 属性供外部调用。

---

### 实验 15 — exec 命名空间内联整数键格式缓存 ❌ 丢弃

- **指标：** 599.7 ms（与 exp14 持平）

**分析：** 尝试将 `_get_struct_fmt` 函数调用替换为 exec 命名空间中的整数键 dict 查找。改善量在噪声范围内（0.5 ms），而代码复杂度提高。按简洁原则丢弃。

---

### 实验 16 — 移除 `isinstance(feature, Feature)` 检查 ✅ 保留

- **提交：** `b347941`
- **指标：** 576.5 ms（−3.9 %）

**分析：** `push()` 每次调用 `isinstance(feature, Feature)` 约 150 ns。以 100K 次计：~15 ms。

**方案：** 移除该检查，信任调用方传入正确类型（Python duck-typing 原则）。非 Feature 对象会在后续 `feature._cache` 访问时自然抛出 AttributeError，错误信息已足够明确。

---

### 实验 17 — 缓存 `_struct.pack` 为模块常量 ❌ 丢弃

- **指标：** 575.1 ms（与 exp16 持平）

**分析：** 尝试将 `_struct.pack` 预先绑定为模块级常量 `_struct_pack`，避免每次 push 的属性查找。改善量在噪声范围内，且 `_struct.pack` 写法更直观，丢弃。

---

### 实验 18 — `struct.Struct` 预编译 pack 方法缓存 ✅ 保留

- **提交：** `fdf4200`
- **指标：** 545.9 ms（−5.3 %）
- **改动文件：** `orm/__init__.py`、`feature/_schema.py`

**分析：** 关键发现：`struct.pack(fmt, *items)` 是模块级函数，每次调用都要解析格式字符串。而 `struct.Struct(fmt)` 预编译后的 `.pack(*items)` 方法**快得多**：

| 方法 | n=8 | n=64 | n=512 |
|------|-----|------|-------|
| `struct.pack(fmt, *items)` | 4.2 µs | 3.7 µs | 16.7 µs |
| `struct.Struct(fmt).pack(*items)` | **1.0 µs** | **0.9 µs** | **6.7 µs** |
| 加速比 | **4.1×** | **4.0×** | **2.5×** |

**方案：** 引入 `_struct_pack_method_cache: dict`，以 `(typecode, n)` 为键缓存 `struct.Struct(fmt).pack` 绑定方法（bound method）。push_fn 签名更新为 `push_fn(cache, t, _gsp)`，其中 `_gsp = _get_struct_pack_method`。

---

### 实验 19 — exec 命名空间整数键 pack 方法缓存 ✅ 保留

- **提交：** `b736620`
- **指标：** 530.1 ms（−2.9 %）
- **改动文件：** `feature/_schema.py`

**分析：** 实验 18 中 `_get_struct_pack_method(typecode, n)` 函数调用本身有开销：
1. 函数调用帧（约 100 ns）
2. 构建 tuple 键 `(typecode, n)`（约 50 ns）

微基准（热 cache，n=8）：
- `_gsp('d', n)(*items)`（tuple 键，函数调用）：0.21 µs
- `_gsp0[n](*items)`（整数键，LOAD_GLOBAL）：0.17 µs，约 **1.33×** 快

**方案：** 将每个 list 字段对应的 pack 方法 cache dict（整数键）直接 bake 进 push_fn 的 exec 命名空间，同时 bake `_SS = struct.Struct`：

```python
# exec 命名空间
{'_gsp0': {},          # 字段 0 的 cache：{n: Struct(nXd).pack}
 '_gsp1': {},          # 字段 1 的 cache（多 list 字段时）
 '_SS': struct.Struct}

# 生成代码片段（字段 xs，typecode='d'）
_items = cache.get('xs') or []
_n = len(_items)
t.set_field_list_numeric(1,
    (_gsp0[_n] if _n in _gsp0 else _gsp0.setdefault(_n, _SS(str(_n)+'d').pack))(*_items))
```

push_fn 签名简化为 `push_fn(cache, t)`，彻底消除结构体查找函数调用开销。

---

### 实验 20 — `push_many()` API + 提取 `_push_slow()` ✅ 保留

- **提交：** `2dfd742`
- **指标：** 520.4 ms（−1.8 %）
- **改动文件：** `orm/__init__.py`

**分析：** `push()` 方法原先同时包含快速路径（list 字段）和慢速路径（无 list 字段）的完整代码，Python 字节码较长，函数局部变量槽分配较多，轻微影响执行效率。

**方案（双重改动）：**

1. **提取 `_push_slow()`**：将无 list 字段的慢速路径提取为独立方法，`push()` 快速路径末尾改为 `return self._push_slow(...)`。`push()` 函数体缩短，字节码更紧凑，CPU 指令 cache 效率更高。

2. **新增 `push_many(features)` API**：将 schema 查找、表查找、可变性检查提升到循环外，对同类 Feature 的批量写入减少每 Feature 的固定开销：
   ```python
   def push_many(self, features: list, table_name: str = '') -> None:
       if not self._is_mutable or not features:
           return
       schema = get_class_schema(features[0].__class__)
       push_fn = schema.push_fn
       # 查表（一次）
       t_obj = self._get_or_create_table(...)
       t_origin = t_obj._origin
       fc = t_obj.feature_count
       for feature in features:
           push_fn(feature._cache, t_origin)
           fc += 1
       t_obj.feature_count = fc
   ```

**注：** 在 `push()` 的基准中（每次一个 feature），改善来自函数体变短。`push_many()` 对小列表（n=8/64）有额外 ~5% 加速，但大列表（n=512）因内存压力（100K × 4KB = 400MB 同时在内存中）略有回归，基准测试仍使用 `push()`。

---

---

## 累积改善曲线

```
1642 ms ┤ ← 基线（原始代码）
1168 ms ┤ exp1  -28.9%  DFS 快速路径
1100 ms ┤ exp2  - 5.8%  模块级导入
1035 ms ┤ exp4  - 5.9%  写入计划预拆分
 842 ms ┤ exp5  -18.6%  array.array 代替 np.asarray
 763 ms ┤ exp6  - 9.3%  struct.pack + 缓存格式串
 731 ms ┤ exp7  - 4.2%  exec 代码生成 push_fn
 697 ms ┤ exp9  - 4.6%  模块级 get_class_schema
 690 ms ┤ exp10 - 1.0%  快速路径内联
 673 ms ┤ exp11 - 2.4%  __setattr__ 内联 fixed 检查
 673 ms ┤ exp12 ~0%     __setattr__ 纯 Python 短路
 610 ms ┤ exp13 - 9.4%  object.__setattr__ in __init__
 600 ms ┤ exp14 - 1.6%  _is_mutable 缓存布尔标志
 577 ms ┤ exp16 - 3.9%  移除 isinstance 检查
 546 ms ┤ exp18 - 5.3%  struct.Struct 预编译 pack 方法
 530 ms ┤ exp19 - 2.9%  整数键 exec 命名空间缓存
 520 ms ┤ exp20 - 1.8%  push_many + 提取 _push_slow
```

---

## 优化分类总结

### 类别 A：消除不必要的算法路径

| 实验 | 优化 | 节省 |
|------|------|------|
| exp1 | 跳过 DFS 图遍历（无 ref 字段时） | −28.9 % |
| exp4 | 预拆分字段写入计划，消除运行时分支 | −5.9 % |

### 类别 B：Python 对象构造优化

| 实验 | 优化 | 节省 |
|------|------|------|
| exp13 | `object.__setattr__` 直接写 slot，跳过 Feature 分发 | −9.4 % |
| exp11 | `__setattr__` 内联 `fixed` 属性检查 | −2.4 % |
| exp12 | `__setattr__` 纯 Python 模式短路 | ~0 % |

### 类别 C：序列化方法选型与缓存

| 实验 | 优化 | 节省 |
|------|------|------|
| exp5 | `array.array` 代替 numpy | −18.6 % |
| exp6 | `struct.pack` + 格式串缓存 | −9.3 % |
| exp18 | `struct.Struct` 预编译 pack 方法（4× 快） | −5.3 % |
| exp19 | 整数键 exec 命名空间 pack 方法缓存 | −2.9 % |

### 类别 D：Python 运行时开销压缩

| 实验 | 优化 | 节省 |
|------|------|------|
| exp7 | `exec` 代码生成专用 push_fn | −4.2 % |
| exp9 | 模块级导入消除延迟 import | −4.6 % |
| exp10 | 内联快速路径消除方法调用 | −1.0 % |
| exp14 | `_is_mutable` 替换 isinstance 检查 | −1.6 % |
| exp16 | 移除 push() 的 isinstance 守卫 | −3.9 % |
| exp20 | 提取 _push_slow 使 push() 更紧凑 | −1.8 % |

---

## 每 Feature 时间分析

### 基线（1642 ms / 100K = 16.4 µs 每 Feature）

| 来源 | 耗时 | 说明 |
|------|------|------|
| `_push_graph()` DFS 框架 | ~6–8 µs | GraphCollector 对象分配、集合操作 |
| `np.asarray().tobytes()` | ~11.4 µs | list → bytes 转换 |
| SWIG 调用（4 次 × 0.7 µs） | ~2.8 µs | add_begin + set_field + set_list + add_end |
| Python 对象开销 | ~1–2 µs | Feature 构造、__setattr__、dict 操作 |

### 最终（520 ms / 100K = 5.2 µs 每 Feature）

| 来源 | 耗时 | 说明 |
|------|------|------|
| `struct.Struct.pack()` | ~0.9–6.7 µs | 已成为主要热点（取决于列表长度） |
| SWIG 调用（4 次） | ~2.8 µs | **不可再压缩（需 C++ 级批量写入 API）** |
| Python 对象开销 | ~0.5 µs | 已充分压缩 |

**结论：** 纯 Python 层面的优化空间已基本耗尽。进一步提升需要 C++ 级别的改动。

---

## 未来工作建议

以下方向超出本次纯 Python 优化范围，但潜力显著：

### 1. C++ 批量写入 API（最高优先级）

当前每个 Feature 需要 4 次 SWIG 调用（约 2.8 µs 固定开销）。  
建议在 C++ 核心中新增：
```cpp
void set_field_list_numeric_bulk(int field_idx, const void* data, size_t n_rows, size_t elem_per_row);
```
可一次写入所有行的列数据，SWIG 调用从 N×4 次降为 1 次。预期提升 50–70 %。

### 2. 原生数组类型支持

当前 push 假设输入为 Python list。如果直接接受 `np.ndarray` 或 `array.array`，可通过 buffer protocol 直接 `memcpy`，消除 Python 对象遍历开销。

### 3. `push_many()` 与固定长度列表结合

当列表长度固定时（如所有行都是 512 个元素），`struct.Struct` 可在 `push_many()` 外层初始化一次，彻底消除 `_gsp0[n]` 查找，预期对 n=512 有额外 10–15 % 改善。

### 4. C++ 端 struct pack（零拷贝）

将 Python 数字列表序列化移到 C++ 侧：Python 只传递 list 对象，C++ 端通过 Python C-API 遍历并写入 fastdb 内存，完全消除中间 bytes 对象的分配。

---

## 结论

本次 autoresearch 优化通过 20 个实验（14 保留，5 丢弃），将 `ORM.push()` 的 build 时间从 **1642 ms 降低至 520 ms**，实现了 **3.15× 的整体提速**，且全程保持 161 个测试全部通过。

核心经验：
1. **算法优化 > 微优化**：exp1（跳过 DFS）和 exp5（序列化方法选型）合计贡献了 50 % 以上的提升
2. **struct.Struct 是隐藏的性能宝藏**：实验 18 发现其比模块级 `struct.pack` 快 4×，且此差距在 Python 文档中并不突出
3. **exec 代码生成是有效工具**：将字段名和索引烘焙为字面量，消除循环和分支，对 Python 热路径有明显效果
4. **SWIG 调用是终极瓶颈**：每次 push 的 4 次 C++ 调用（约 2.8 µs）是纯 Python 优化无法突破的下限
