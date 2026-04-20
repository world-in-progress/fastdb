# 设计文档：scalar Feature push() 快速路径优化

**日期**：2026-04-02  
**范围**：`python/fastdb4py/orm/__init__.py` · `python/fastdb4py/feature/_schema.py`  
**关联报告**：`docs/opt/kostya-benchmark-report.md` · `docs/opt/orm-push-optimization-report.md`

---

## 问题陈述

Kostya benchmark（N=100K）显示 fastdb build 耗时 **1146ms**（11.2 µs/record），
主要来自 `_push_slow()` 的 Python 循环 + 4× SWIG `set_field` 调用。

### 根因：push() 路由逻辑存在漏洞

`_compile_push_fn()` **对所有 Feature 类型**（numeric + str + bytes + list）都生成专用函数，
但 `push()` 仅对含 list 字段的 Feature 使用 `push_fn` 快速路径：

```python
# orm/__init__.py:264  ← 当前逻辑
if schema.list_element_types:      # 有 list 字段 → 快速路径
    schema.push_fn(feature._cache, t_obj._origin)
else:
    return self._push_slow(...)    # 无 list 字段 → 慢路径（即使 push_fn 已就绪）
```

对 `Coord { row_id:U32, x:F64, y:F64, z:F64, name:STR }`：
- `list_element_types` = `{}` → 走慢路径
- 但 `schema.push_fn` 已生成，包含 `add_feature_begin/end + 4×set_field + 1×set_field_cstring`
- `push_many()` 已经直接使用 `push_fn`（正确），只有 `push()` 没有

### 数据佐证

```
操作                        µs/record
────────────────────────────────────
Feature init + 5 setattr     1.21      ← Python 对象开销
push() 路由 + 表管理         0.40
_push_slow Python 循环       3.90      ← 可消除
SWIG set_field × 4           5.12      ← 不可避免（调用税）
SWIG set_field_cstring × 1   0.80
────────────────────────────────────
总计                        11.43 µs
```

消除 `_push_slow` Python 循环（3.90 µs）预期节省 **34%**。

---

## 优化方案 B（本次实施）

### 方案 B1：修改 push() 路由条件（核心变更）

**变更位置**：`orm/__init__.py::push()`

```python
# 当前（错误路由）
if schema.list_element_types:
    # fast path for list features only
    ...
    schema.push_fn(feature._cache, t_obj._origin)
    t_obj.feature_count += 1
    return
return self._push_slow(feature, ...)   # scalar features 都走这里

# 优化后（正确路由）
if not schema.has_ref_fields:
    # fast path for ALL non-ref features (scalar + str + bytes + list)
    feat_table_name = table_name or feature_type.__name__
    t_obj = self._table_map.get(feat_table_name)
    if t_obj is None:
        t_obj = _create_table(self, feature_type, feat_table_name, schema)
    schema.push_fn(feature._cache, t_obj._origin)
    t_obj.feature_count += 1
    ...
    return
# ref fields → graph traversal (unchanged)
return self._push_graph(feature, ...)
```

### 方案 B2：内联 push_fn 调用，消除 Table.push2 contextmanager 开销

`_push_slow` 使用 `with Table.push2(table) as t:` contextmanager，
每次调用有 `contextlib.__enter__/__exit__` 开销（约 0.5 µs/call）。
`push_fn` 直接在函数内调用 `t.add_feature_begin/end()`，绕过 contextmanager。

### 表初始化统一（重要）

当前快速路径的表初始化代码（处理 list 字段）与 `_push_slow` 的表初始化（只处理标量）逻辑不同：

```python
# 快速路径（含 list 字段处理）—— 可复用于 scalar
for fn, ft in schema.ordered_defns:
    if ft == OriginFieldType.list:
        new_table._origin.add_list_field(fn, cpp_elem)
    else:
        new_table._origin.add_field(fn, ft.value)
```

对 scalar feature，`ft == OriginFieldType.list` 分支从不触发，行为与 `_push_slow` 的初始化等价。

---

## 预期收益

| 指标 | 当前 | 优化后（估算） |
|------|------|--------------|
| µs/record | 11.4 | ~7.5 |
| N=100K build | 1146ms | ~750ms |
| 加速比 | 1× | ~1.5× |

*实际收益由 autoresearch 实验确定。*

---

## 方案 A（后续评估）：truncate 支持 STR 字段

方案 B 后，如果仍需进一步提速，方案 A 提供 80-200× 的加速：

**设计思路**：`ORM.truncate()` 扩展，接受 `list[str]` 批量 STR 编码：

```python
# 新增：ORM.build_from_arrays(cls, **field_arrays)
orm = ORM.build_from_arrays(
    Coord,
    row_id=np.arange(N, dtype=np.uint32),
    x=xs, y=ys, z=zs,
    name=names,        # list[str] — Python 侧批量编码后写入
)
```

实现路径：
1. 数值字段：直接 NumPy memcpy（走 truncate+fill 路径）
2. STR 字段：Python 侧构建 `str → u16` 映射，生成索引数组，调用新增 SWIG 方法批量写入

**STR 批量写入的 SWIG 接口**（待设计）：
```python
# 新增 SWIG 方法（概念）
layer.set_column_cstrings(field_idx, list_of_strings)
```

> **本次不实施**：方案 A 需要 C++/SWIG 接口变更，复杂度高。
> 先通过 autoresearch 验证方案 B 的实际收益，再决定是否继续。

---

## autoresearch 配置

**目标指标**：`tests/python/benchmark_kostya.py --n 100000 --reps 3` 中 fastdb `build_ms`

**初始值**：~1146ms

**实验方向**（供 autoresearch 探索）：
1. 修改 push() 路由条件（B1）
2. 消除 contextmanager 开销（B2）
3. 进一步内联表查找缓存
4. push_fn 生成优化（消除 `cache.get()` None 检查）

---

*本设计文档由 brainstorming session 生成，autoresearch 实验结果将更新至 `docs/opt/` 目录*
