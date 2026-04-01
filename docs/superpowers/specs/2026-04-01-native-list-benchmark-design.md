# Native List Columns Benchmark Design

**Goal:** Compare fastdb native list columns against PyArrow and pickle across write, share/serialize, and read phases, across varying N and list lengths.

---

## Scenarios

Three implementations under test:

| ID | System | Write | Publish | Read |
|---|---|---|---|---|
| `fastdb` | fastdb ORM + native list cols | `ORM.create()` → `push(feat)` × N | `orm.share(shm_name)` (POSIX shm) | `ORM.load()` → `feat.xs` (zero-copy NumPy) |
| `arrow` | PyArrow Table + IPC | `pa.array([…])` → `pa.table(…)` | `pa.ipc.new_stream(buf_sink)` → write to shm | read from shm → `pa.ipc.open_stream` → column |
| `pickle` | Python list-of-dicts + pickle | build list of dicts `{id, xs}` | `pickle.dumps()` → write to shm | read from shm → `pickle.loads()` → iterate |

---

## Matrix

- **N** ∈ {10_000, 100_000, 1_000_000}
- **list_len** ∈ {8, 64, 512}
- **repetitions**: 3 per cell, report median

---

## Metrics (per cell)

| Metric | Description |
|---|---|
| `build_ms` | Time to construct in-memory structure (Feature objects / pa.table / list-of-dicts) |
| `serialize_ms` | Time to write to shared memory / IPC buffer |
| `deserialize_ms` | Time to load from shared memory / IPC buffer |
| `read_col_ms` | Time to iterate all N rows and access the `xs` column (force materialization) |
| `total_ms` | sum of all four phases |
| `throughput_mb_s` | total data bytes / total_ms |

---

## Data model

Each row has:
- `id: U32 / int` — a scalar identity field
- `xs: List[F64]` — the variable-length list column, length = `list_len`

For fastdb:
```python
class BenchListFeature(Feature):
    id: U32
    xs: List[F64]
```

For PyArrow:
```python
schema = pa.schema([("id", pa.uint32()), ("xs", pa.list_(pa.float64()))])
```

For pickle: `[{"id": i, "xs": [float(j) for j in range(list_len)]} for i in range(N)]`

---

## File location

`tests/python/benchmark_native_list.py`

Follows existing benchmark conventions (argparse `--quick`, `--output-json`, timer decorator, results table).

---

## Output format

```
Native List Column Benchmark
N=10000  list_len=8
┌──────────────┬───────────┬──────────────┬────────────────┬─────────────┬───────────┬─────────────────┐
│ system       │ build_ms  │ serialize_ms │ deserialize_ms │ read_col_ms │ total_ms  │ throughput MB/s │
├──────────────┼───────────┼──────────────┼────────────────┼─────────────┼───────────┼─────────────────┤
│ fastdb       │     12.3  │        4.1   │          2.8   │        1.5  │      20.7 │          308.0  │
│ arrow        │      8.2  │        2.4   │          1.3   │        0.9  │      12.8 │          497.0  │
│ pickle       │      5.1  │       18.3   │         14.2   │        9.4  │      47.0 │          135.0  │
└──────────────┴───────────┴──────────────┴────────────────┴─────────────┴───────────┴─────────────────┘
```

---

## Notes

- Skip N=1_000_000 in `--quick` mode
- Install pyarrow if not present, skip `arrow` with clear warning if unavailable
- Cleanup shm segments in `finally` blocks
- Module-level Feature class (`BenchListFeature`) to ensure `get_type_hints()` resolves correctly
