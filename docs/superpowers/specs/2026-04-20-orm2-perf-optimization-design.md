# ORM2 Push Performance Optimization

## Problem

ORM2 (decorator-based `@feature` ORM) push path is ~4x slower than old ORM (Feature subclass ORM) due to:
- Per-push `get_schema()` WeakKeyDict lookup
- Per-push `ref_resolver` closure creation
- `_set_field()` if/elif dispatch per field
- SWIG wrapper overhead (~200ns/call) instead of direct C extension calls
- No batch buffering — every push immediately calls C++

Read path is already 27% faster than old ORM. Only push path needs optimization.

## Goal

Match or exceed old ORM push performance while maintaining ORM2's clean decorator API.

## Approach: Full Deferred Batch Push

All serialization is deferred to `combine()` time. `push()` becomes a pure Python append.

## Architecture

```
push(obj) ──► dedup(id) ──► _pending.append(obj)
                               _pushed_objs keeps obj alive

combine() ──► group by class
           ──► build class dep graph (REF fields)
           ──► topological sort classes
           ──► for each class (topo order):
                 no REF? → push_many_from_dicts_fc (1 C call / 1024 dicts)
                 has REF? → compiled push_fn + inline ref resolution
           ──► finalize DB (post → load_xbuffer)
```

### New Module: `push_compiler.py`

Extracted from `feature/_schema.py` to share between old ORM and ORM2:

- `compile_push_fn(numeric_plan, str_plan, bytes_plan, list_plan)` — exec-generated zero-branch per-class push function
- `make_inlined_dispatch(...)` — `partial(push_from_dict_fc, ...)` for simple features
- `make_batch_dispatch(...)` — `partial(push_many_from_dicts_fc, ...)` for batch mode
- Direct C extension references (`_c_add_begin`, `_c_set_field`, etc.) — bypass SWIG wrapper

### Modified Files

| File | Change |
|------|--------|
| `push_compiler.py` | NEW — extracted push optimization functions |
| `orm2.py` | Deferred batch push + topo sort combine |
| `push.py` | Use push_compiler's C extension refs |
| `registry.py` | LayerSchema gains compiled plans/push_fn |
| `feature/_schema.py` | Import from push_compiler instead of inline |

## Public API Changes

```python
class ORM2:
    def push(self, obj) -> None:     # was -> int, now -> None
    def combine(self):               # unchanged signature, new batch internals
    def count(self, cls) -> int:     # before combine: pending count
    # get/iter/share/load/unlink — unchanged
```

- `push()` returns `None` (no current code uses the return value)
- Object mutation after push affects final value (matches old ORM batch semantics)
- `count()` before `combine()` returns pending object count

## Combine-Time Batch Push Detail

### Non-REF Classes (fast path)

```python
# Group objects by class → extract __dict__ list → batch C call
dicts = [obj.__dict__ for obj in group]
for batch in chunks(dicts, 1024):
    schema.batch_fn(batch)  # push_many_from_dicts_fc
```

One C call processes up to 1024 feature dicts. This is the same path old ORM uses via `_flush_push_batches()`.

### REF Classes (compiled path)

```python
for obj in group:
    cache = obj.__dict__.copy()  # COPY — don't mutate original object
    # Inline REF resolution: replace Python objects with WxFeatureRef
    for fd in schema.ref_fields:
        ref_obj = cache.get(fd.name)
        if ref_obj is not None:
            li, ri = obj_to_row[id(ref_obj)]
            cache[fd.name] = WxFeatureRef.make_ref(li, ri)
    # LIST[REF] resolution: pack as 5-byte structs
    for fd in schema.list_ref_fields:
        items = cache.get(fd.name)
        if items:
            parts = []
            for ref_obj in items:
                li, ri = obj_to_row[id(ref_obj)]
                parts.append(struct.pack('<HBH', li, ri & 0xFF, ri >> 8))
            cache[fd.name] = b''.join(parts)
    schema.push_fn(cache, layer_build)
```

REF fields are resolved by looking up `obj_to_row[id(ref_obj)]` — O(1) hash lookup. The compiled `push_fn` then handles the actual C calls with zero branch overhead.

### Topological Sort

Classes are sorted by their REF field dependencies:

```python
# Build: cls → set of classes it depends on
class_deps = {cls: set() for cls in groups}
for cls, schema in schemas:
    for fd in schema.fields:
        if fd.field_type == ref and fd.ref_target_cls in groups:
            class_deps[cls].add(fd.ref_target_cls)
```

Kahn's algorithm for topo sort. Circular class dependencies (A refs B, B refs A) are detected and raise a clear error.

Note: `@feature` decorator already records `ref_target_cls` and `list_ref_target_cls` on FieldDef — no new metadata needed.

## Push Compiler Extraction

### Functions to extract from `feature/_schema.py`:

1. **C extension references** (lines 12-23):
   ```python
   _c_add_begin = _fdb_c.WxLayerTableBuild_add_feature_begin
   _c_add_end   = _fdb_c.WxLayerTableBuild_add_feature_end
   # ... etc
   ```

2. **`_compile_push_fn()`** (lines 124-163): exec-generated push function

3. **`make_inlined_dispatch()`** (lines 166-226): partial-based single dispatch

4. **`make_batch_inlined_dispatch()`** (lines 228-250): batch dispatch

### `feature/_schema.py` after extraction:

```python
from ..push_compiler import (
    compile_push_fn as _compile_push_fn,
    make_inlined_dispatch, make_batch_inlined_dispatch,
    _c_add_begin, _c_add_end, _c_set_field, _c_set_cstr,
    _c_set_wstr, _c_set_raw, _c_set_list,
    _c_push_dict, _c_pfd_fc, _c_pmfd_fc,
)
```

All behavior is preserved — only import source changes.

## Registry Enhancement

`LayerSchema` in `registry.py` gains:

```python
@dataclass
class LayerSchema:
    # ... existing fields ...
    numeric_plan: list = None    # [(idx, field_name), ...]
    str_plan: list = None        # [(idx, field_name, is_wide), ...]
    bytes_plan: list = None      # [(idx, field_name), ...]
    list_plan: list = None       # [(idx, field_name, typecode), ...]
    push_fn: Callable = None     # compiled zero-branch push function
    batch_fn: Callable = None    # batch push (push_many_from_dicts_fc partial)
    ref_fields: list = None      # [FieldDef] where field_type == ref
    list_ref_fields: list = None # [FieldDef] where list_elem_type == ref
```

These are split across two phases:
- **Plans** (numeric_plan, str_plan, etc.) and **field lists** (ref_fields, list_ref_fields): computed once in `@feature` decorator at class definition time
- **Compiled functions** (push_fn, batch_fn): created at `combine()` time when `layer_build` objects exist, then cached for the combine session

## Testing

### Existing tests (must all pass)

All 215 existing tests continue to pass — API behavior unchanged.

### New tests

| Test | Description |
|------|-------------|
| `test_batch_correctness` | Push 1000 simple objects, verify all column values |
| `test_ref_topo_sort` | A→B→C dependency chain, verify push order |
| `test_dedup_batch` | Push same object twice, verify single row |
| `test_mutation_after_push` | Modify object after push, verify combine uses latest value |
| `test_perf_parity` | Benchmark: ORM2 push within ±20% of old ORM |

## Performance Expectations

| Scenario | Old ORM | ORM2 (current) | ORM2 (optimized) |
|----------|---------|-----------------|-------------------|
| 2000 simple push | 3.25ms | 13.4ms | ~3ms (batch path) |
| 2000 mixed push | 2.57ms | 10.4ms | ~2.5ms (batch path) |
| 2000 REF push | ~10ms | ~25ms | ~12ms (compiled fn) |
