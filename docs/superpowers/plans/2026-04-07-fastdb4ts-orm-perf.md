# fastdb4ts ORM/Layer/Feature Performance Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Python/C++ 侧已完成的 orm→layer→feature 路径优化逐一移植到 TypeScript binding，消除热路径中不必要的 WASM 函数调用。

**Architecture:** TypeScript 层通过 Emscripten/Embind 访问 WASM；每次跨越 JS↔WASM 边界的调用开销约 200–500 ns。优化核心是：能用 DataView 直接读写 WASM 线性内存的地方就不走 WASM 函数，必须走 WASM 的地方就批量合并为一次调用。

**Tech Stack:** TypeScript 5.x, Emscripten/Embind, Node.js `node:test`, DataView/TypedArray

---

## 背景：Python 侧已完成的优化与 TS 侧现状对照

| Python 轮次 | 内容 | TS 现状 |
|---|---|---|
| o1 | ColumnAccessor O(1) 字段查找 | ✅ `schema.fieldMap.get()` 已是 O(1) |
| o1.1 | ColumnAccessor numpy 数组缓存（避免每次 SWIG get_column）| ❌ `table.column.x` 每次新建 `StridedColumn` |
| o2 | `_cache` 懒分配 | ✅ 已有 `_cache = null` + `_getCache()` |
| o3 | `iter_reuse()` 复用 Feature 实例 | ✅ 已有 `iterReuse()` |
| o4 | `Table.fill()` 批量列写入 | ✅ 已有 `fill(columns)` |
| o5 | 统一 ClassSchema（4 WeakKeyDict → 1）| ✅ 已用单 WeakMap |
| o6 | 双层 Schema 缓存（class attr + WeakMap）| ❌ 仅有 WeakMap |
| o7 | `read_all_scalars`/`write_all_scalars` 批量字段 API | ❌ WASM 侧已绑定但 TS 层未暴露 |
| —  | `StridedColumn.set/fill` 仍走 WASM 函数 | ❌ `set(i,v)` = 2 次 WASM 调用/元素 |
| —  | `ORM.push()` 纯数值 Feature 逐字段 WASM 调用 | ❌ N_fields 次 WASM 调用/feature |

---

## 文件变更总览

| 文件 | 操作 | 职责 |
|---|---|---|
| `tests/ts/bench_orm.mjs` | 新建 | ORM 性能基准（3 节：微/中/宏）|
| `ts/fastdb4ts/src/table.ts` | 修改 | `column` proxy 缓存 `StridedColumn` 实例（OPT-TS-1）|
| `ts/fastdb4ts/src/column.ts` | 修改 | `set()`/`fill()` 改用 DataView 直写堆内存（OPT-TS-2）|
| `ts/fastdb4ts/src/schema.ts` | 修改 | `ClassSchema` 新增 `numericFieldCount`；`getClassSchema` 写 class 属性缓存（OPT-TS-3/4）|
| `ts/fastdb4ts/src/feature.ts` | 修改 | 新增 `readAllScalars(out?)`/`writeAllScalars(values)`（OPT-TS-4）|
| `ts/fastdb4ts/src/orm.ts` | 修改 | `push()` 纯数值路径改用 `setFieldsFromHeap` 批写（OPT-TS-5）|
| `ts/fastdb4ts/src/index.ts` | 修改 | 导出新增公开 API（如有）|
| `CHANGELOG.md` | 修改 | 在 `fastdb4ts` section 添加 Performance 条目 |

---

## Task 1: Benchmark 基础设施

**Files:**
- Create: `tests/ts/bench_orm.mjs`

建立可量化的性能基线，后续每轮优化后运行对比。

- [ ] **Step 1: 新建 bench_orm.mjs 骨架**

```js
// tests/ts/bench_orm.mjs
import { initFastdb, ORM, TableDefn, Feature, defineSchema, F64, I32, STR } from '../../ts/fastdb4ts/dist/index.js';

await initFastdb();

function bench(label, fn, warmup = 10, iters = 200) {
  for (let i = 0; i < warmup; i++) fn();
  const times = [];
  for (let i = 0; i < iters; i++) {
    const t0 = performance.now();
    fn();
    times.push((performance.now() - t0) * 1e3); // µs
  }
  times.sort((a, b) => a - b);
  const med = times[Math.floor(iters / 2)];
  const p95 = times[Math.floor(iters * 0.95)];
  console.log(`${label.padEnd(50)} median=${med.toFixed(2)}µs  p95=${p95.toFixed(2)}µs`);
}

class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64, z: F64 });
}
```

- [ ] **Step 2: 添加 Section 1 — 微基准**

```js
// --- Section 1: Microbenchmarks ---
console.log('\n=== Section 1: Microbenchmarks ===');

const orm1 = ORM.truncate([new TableDefn(Point, 1)]);
const tbl1 = orm1.table(Point);
const pt = tbl1.get(0);
pt.x = 1.0; pt.y = 2.0; pt.z = 3.0;

bench('scalar_read_db_mapped (F64)', () => { const _ = pt.x; });
bench('scalar_write_db_mapped (F64)', () => { pt.x = 1.5; });
bench('feature_init_db_mapped', () => { tbl1.get(0); });
bench('iterReuse N=100 (per iter)', (() => {
  const orm = ORM.truncate([new TableDefn(Point, 100)]);
  const tbl = orm.table(Point);
  return () => { for (const _ of tbl.iterReuse()) {} };
})());
bench('column.x access (StridedColumn create)', () => { const _ = tbl1.column.x; });
```

- [ ] **Step 3: 添加 Section 2 — 中基准**

```js
// --- Section 2: Meso-benchmarks ---
console.log('\n=== Section 2: Meso-benchmarks ===');

bench('ORM.truncate N=100', () => {
  ORM.truncate([new TableDefn(Point, 100)]);
});

const xs = new Float64Array(1000).fill(1.5);
const ormFill = ORM.truncate([new TableDefn(Point, 1000)]);
const tblFill = ormFill.table(Point);
bench('StridedColumn.fill N=1000 (1 col)', () => { tblFill.column.x.fill(xs); });
bench('Table.fill N=1000 (3 cols)', () => { tblFill.fill({ x: xs, y: xs, z: xs }); });

bench('StridedColumn.set single', () => { tblFill.column.x.set(0, 3.14); });
bench('StridedColumn.toArray N=1000', () => { tblFill.column.x.toArray(); });
bench('ORM.push N=100', (() => {
  const p = new Point({ x: 1, y: 2, z: 3 });
  return () => {
    const o = ORM.create();
    for (let i = 0; i < 100; i++) o.push(p);
    o.combine();
  };
})());
```

- [ ] **Step 4: 添加 Section 3 — 宏基准**

```js
// --- Section 3: Macro-benchmarks ---
console.log('\n=== Section 3: Macro-benchmarks ===');

const N = 1000;
const ormBig = ORM.truncate([new TableDefn(Point, N)]);
const tblBig = ormBig.table(Point);
const bigArr = Float64Array.from({ length: N }, (_, i) => i * 0.1);
tblBig.fill({ x: bigArr, y: bigArr, z: bigArr });

bench('point_cloud_read_rowwise N=1000', () => {
  let sum = 0;
  for (let i = 0; i < N; i++) sum += tblBig.get(i).x;
});
bench('point_cloud_read_iterReuse N=1000', () => {
  let sum = 0;
  for (const pt of tblBig.iterReuse()) sum += pt.x;
});
bench('point_cloud_read_columnwise N=1000', () => {
  const arr = tblBig.column.x.toArray();
  let sum = 0; for (let i = 0; i < arr.length; i++) sum += arr[i];
});
bench('point_cloud_write_columnwise N=1000', () => {
  tblBig.column.x.fill(bigArr);
});
bench('point_cloud_write_fill N=1000 (3 cols)', () => {
  tblBig.fill({ x: bigArr, y: bigArr, z: bigArr });
});

console.log('\nDone.');
```

- [ ] **Step 5: 在 package.json 添加 bench 脚本**

在 `ts/fastdb4ts/package.json` 的 `scripts` 中添加：
```json
"bench:orm": "npm run build && node ../../tests/ts/bench_orm.mjs"
```

- [ ] **Step 6: 运行基准，保存基线数据**

```bash
npm --prefix ts/fastdb4ts run bench:orm
```

记录输出作为基线（后续对比用）。

- [ ] **Step 7: Commit**

```bash
git add tests/ts/bench_orm.mjs ts/fastdb4ts/package.json
git commit -m "bench(ts): add bench_orm.mjs ORM performance baseline

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

<!-- END_TASK_1 -->

## Task 2: OPT-TS-1 — `Table.column` 缓存 `StridedColumn` 实例（移植 Python o1.1）

**Files:**
- Modify: `ts/fastdb4ts/src/table.ts`
- Test: `tests/ts/test_column_way.mjs`（已有，补充断言）

**问题**：`table.column` 的 Proxy `get` 每次都 `new StridedColumn(...)`，其构造器含 3 次 WASM 调用（`getFieldOffset` + `tryGetFeatureAt(0)` + `getAddress()`）。固定表列地址不变，缓存安全。

- [ ] **Step 1: 写失败测试**

在 `tests/ts/test_column_way.mjs` 补充：
```js
test('table.column.x returns cached StridedColumn instance', () => {
  const db = ORM.truncate([new TableDefn(Point, 5)]);
  const tbl = db.table(Point);
  const col1 = tbl.column.x;
  const col2 = tbl.column.x;
  assert.strictEqual(col1, col2, 'expected same StridedColumn instance');
});
```

- [ ] **Step 2: 运行，确认失败**

```bash
npm --prefix ts/fastdb4ts run test:ts 2>&1 | grep -A3 "cached StridedColumn"
```
Expected: FAIL（每次新建实例）

- [ ] **Step 3: 修改 `table.ts` — Proxy 闭包内增加 Map 缓存**

定位 `get column()` 中的 `new Proxy({}, { get(_, prop) { ... return new StridedColumn(...) } })` 块，替换为：
```ts
const columnCache = new Map<string, StridedColumn>();
const proxy = new Proxy(
  {},
  {
    get(_, prop) {
      if (typeof prop !== 'string') return undefined;
      const existing = columnCache.get(prop);
      if (existing !== undefined) return existing;
      const def = getFieldDefinition(schema, prop);
      if (!def) return undefined;
      const col = new StridedColumn(table.module, layer, def.index, def.entry.originType);
      columnCache.set(prop, col);
      return col;
    },
  }
);
```

- [ ] **Step 4: 运行测试，确认通过**

```bash
npm --prefix ts/fastdb4ts run test:ts
```
Expected: 全部 PASS

- [ ] **Step 5: 运行 bench，记录 `column.x access` 变化**

```bash
npm --prefix ts/fastdb4ts run bench:orm 2>&1 | grep "column"
```

- [ ] **Step 6: Commit**

```bash
git add ts/fastdb4ts/src/table.ts tests/ts/test_column_way.mjs
git commit -m "perf(ts): cache StridedColumn instances in Table.column proxy (OPT-TS-1)

Avoids 3 WASM calls per repeated table.column.x access by caching
StridedColumn instances in a closure-scoped Map<string, StridedColumn>.
Safe for fixed tables: field offsets and base addresses are immutable.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

<!-- END_TASK_2 -->

## Task 3: OPT-TS-2 — `StridedColumn.set()` / `fill()` 消除 WASM 调用

**Files:**
- Modify: `ts/fastdb4ts/src/column.ts`
- Test: `tests/ts/test_column_way.mjs`

**问题**：
- `set(i, v)` → `tryGetFeatureAt(i)` + `setFieldDouble(idx, v)` = **2 次 WASM 调用/元素**
- `fill(values)` → N × `set(i, v[i])` = **2N 次 WASM 调用**
- `StridedColumn` 构造器已计算 `basePtr`（WASM 堆绝对地址）和 `stride`
- 读路径已用 `DataView` 直写 WASM 堆内存，写路径可做同样的事

**方案**：`set()` 和 `fill()` 改用 `new DataView(module.HEAPU8.buffer).setFloat64/setInt32/...` 直写堆内存，绕过 WASM 函数调用。

- [ ] **Step 1: 在 `test_column_way.mjs` 补充正确性测试**

```js
test('StridedColumn set/fill round-trip via direct heap write', () => {
  const db = ORM.truncate([new TableDefn(Point, 4)]);
  const tbl = db.table(Point);
  const col = tbl.column.x;

  // single set
  col.set(0, 42.5);
  assert.equal(col.get(0), 42.5);

  // fill
  col.fill([1.1, 2.2, 3.3, 4.4]);
  assert.equal(col.get(0), 1.1);
  assert.equal(col.get(3), 4.4);

  // negative index
  col.set(-1, 99.0);
  assert.equal(col.get(3), 99.0);
});
```

- [ ] **Step 2: 运行，确认当前测试通过（基线正确性）**

```bash
npm --prefix ts/fastdb4ts run test:ts 2>&1 | grep -A3 "round-trip"
```

- [ ] **Step 3: 新增 `writeAt()` 私有方法（替换两处 WASM 调用）**

在 `column.ts` 的 `StridedColumn` 类末尾，`getDataView()` 之前添加：
```ts
private writeAt(byteOffset: number, value: number): void {
  const dv = new DataView(this.module.HEAPU8.buffer);
  if (this.kind === this.module.ftF64) {
    dv.setFloat64(byteOffset, value, true);
  } else if (this.kind === this.module.ftF32 || this.kind === this.module.ftU8n || this.kind === this.module.ftU16n) {
    dv.setFloat32(byteOffset, value, true);
  } else if (this.kind === this.module.ftI32) {
    dv.setInt32(byteOffset, Math.trunc(value), true);
  } else if (this.kind === this.module.ftU32) {
    dv.setUint32(byteOffset, Math.trunc(value) >>> 0, true);
  } else if (this.kind === this.module.ftU16) {
    dv.setUint16(byteOffset, Math.trunc(value) & 0xffff, true);
  } else if (this.kind === this.module.ftU8) {
    dv.setUint8(byteOffset, Math.trunc(value) & 0xff);
  } else {
    throw new FastdbRuntimeError(`Field index ${this.fieldIndex} is not a numeric column.`);
  }
}
```

- [ ] **Step 4: 修改 `set()` — 改用 `writeAt()`**

将原：
```ts
set(index: number, value: number): void {
  const feature = this.layer.tryGetFeatureAt(this.normalizeIndex(index));
  if (
    this.kind === this.module.ftF32 ||
    this.kind === this.module.ftF64 ||
    this.kind === this.module.ftU8n ||
    this.kind === this.module.ftU16n
  ) {
    feature.setFieldDouble(this.fieldIndex, value);
  } else {
    feature.setFieldInt(this.fieldIndex, Math.trunc(value));
  }
}
```
改为：
```ts
set(index: number, value: number): void {
  const normalized = this.normalizeIndex(index);
  this.writeAt(this.basePtr + normalized * this.stride, value);
}
```

- [ ] **Step 5: 修改 `fill()` — 单次 DataView，循环写入**

将原：
```ts
fill(values: ArrayLike<number>): void {
  if (values.length !== this.length) { ... }
  for (let i = 0; i < this.length; i += 1) {
    this.set(i, values[i] ?? 0);
  }
}
```
改为：
```ts
fill(values: ArrayLike<number>): void {
  if (values.length !== this.length) {
    throw new FastdbRuntimeError(
      `Column fill length mismatch: expected ${this.length}, got ${values.length}.`
    );
  }
  const dv = new DataView(this.module.HEAPU8.buffer);
  const stride = this.stride;
  let ptr = this.basePtr;
  if (this.kind === this.module.ftF64) {
    for (let i = 0; i < this.length; i += 1, ptr += stride) {
      dv.setFloat64(ptr, values[i] ?? 0, true);
    }
  } else if (this.kind === this.module.ftF32 || this.kind === this.module.ftU8n || this.kind === this.module.ftU16n) {
    for (let i = 0; i < this.length; i += 1, ptr += stride) {
      dv.setFloat32(ptr, values[i] ?? 0, true);
    }
  } else if (this.kind === this.module.ftI32) {
    for (let i = 0; i < this.length; i += 1, ptr += stride) {
      dv.setInt32(ptr, Math.trunc(values[i] ?? 0), true);
    }
  } else if (this.kind === this.module.ftU32) {
    for (let i = 0; i < this.length; i += 1, ptr += stride) {
      dv.setUint32(ptr, (values[i] ?? 0) >>> 0, true);
    }
  } else if (this.kind === this.module.ftU16) {
    for (let i = 0; i < this.length; i += 1, ptr += stride) {
      dv.setUint16(ptr, (values[i] ?? 0) & 0xffff, true);
    }
  } else if (this.kind === this.module.ftU8) {
    for (let i = 0; i < this.length; i += 1, ptr += stride) {
      dv.setUint8(ptr, (values[i] ?? 0) & 0xff);
    }
  } else {
    throw new FastdbRuntimeError(`Field index ${this.fieldIndex} is not a numeric column.`);
  }
}
```

- [ ] **Step 6: 运行测试，确认仍通过**

```bash
npm --prefix ts/fastdb4ts run test:ts
```
Expected: 全部 PASS

- [ ] **Step 7: 运行 bench，对比 fill/set 指标**

```bash
npm --prefix ts/fastdb4ts run bench:orm 2>&1 | grep -E "fill|set"
```
Expected: `StridedColumn.fill N=1000` 显著降低（0 WASM 调用 vs 2000 WASM 调用）

- [ ] **Step 8: Commit**

```bash
git add ts/fastdb4ts/src/column.ts tests/ts/test_column_way.mjs
git commit -m "perf(ts): StridedColumn.set/fill use direct DataView heap writes (OPT-TS-2)

Eliminate 2 WASM calls per element in set() and 2N WASM calls in fill()
by writing directly to WASM linear memory via DataView. Reads already
used DataView; writes now do the same. Correctness verified by round-trip test.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

<!-- END_TASK_3 -->

## Task 4: OPT-TS-3 — Schema 双层缓存（移植 Python o6）

**Files:**
- Modify: `ts/fastdb4ts/src/schema.ts`

**问题**：`getClassSchema(ctor)` 热路径走 `WeakMap.get(ctor)`，每次 `Feature` 构造和 `Proxy.get` 分支都调用一次。V8 的 WeakMap 访问比直接读 class 属性慢约 3-5×。

**方案**：在 `getClassSchema()` 冷路径额外用 `Object.defineProperty(ctor, '__fdb_schema__', { value, writable: false, configurable: false })` 写入类对象；热路径用 `(ctor as Record<string, unknown>)['__fdb_schema__']` 先查，未命中再走 WeakMap。

- [ ] **Step 1: 写失败测试（验证 schema 对象身份一致）**

新建 `tests/ts/test_schema_cache.mjs`：
```js
import assert from 'node:assert/strict';
import test from 'node:test';
import { Feature, defineSchema, F64, getClassSchema, initFastdb } from '../../ts/fastdb4ts/dist/index.js';

await initFastdb();

class CachePoint extends Feature {
  static schema = defineSchema({ x: F64, y: F64 });
}

test('getClassSchema returns identical object on repeated calls', () => {
  const s1 = getClassSchema(CachePoint);
  const s2 = getClassSchema(CachePoint);
  assert.strictEqual(s1, s2);
});

test('class-level attribute cache is set after first call', () => {
  getClassSchema(CachePoint); // warm
  assert.ok(
    Object.prototype.hasOwnProperty.call(CachePoint, '__fdb_schema__'),
    'Expected __fdb_schema__ on class after getClassSchema()'
  );
});
```

- [ ] **Step 2: 运行，确认第二个测试失败**

```bash
npm --prefix ts/fastdb4ts run build && node --test tests/ts/test_schema_cache.mjs
```
Expected: 第一个 PASS，第二个 FAIL（属性未设）

- [ ] **Step 3: 修改 `schema.ts` — 双层缓存**

在 `getClassSchema` 函数顶部添加快路径，在冷路径末尾写入类属性：
```ts
const SCHEMA_ATTR = '__fdb_schema__';

export function getClassSchema(ctor: FeatureClassLike): ClassSchema {
  // Fast path: class-level attribute (~2× faster than WeakMap in V8)
  const fast = (ctor as Record<string, unknown>)[SCHEMA_ATTR];
  if (fast !== undefined) return fast as ClassSchema;

  // Warm path: WeakMap
  const cached = SCHEMA_CACHE.get(ctor);
  if (cached) return cached;

  // Cold path: compute schema ...
  // (existing field-loop code unchanged)
  // ...

  SCHEMA_CACHE.set(ctor, schema);
  try {
    Object.defineProperty(ctor, SCHEMA_ATTR, {
      value: schema,
      writable: false,
      enumerable: false,
      configurable: false,
    });
  } catch {
    // Sealed / frozen classes — fall back to WeakMap only
  }
  return schema;
}
```

- [ ] **Step 4: 运行测试，确认通过**

```bash
npm --prefix ts/fastdb4ts run build && node --test tests/ts/test_schema_cache.mjs
```
Expected: 全部 PASS

- [ ] **Step 5: 完整测试套件**

```bash
npm --prefix ts/fastdb4ts run test:ts
```
Expected: 全部 PASS

- [ ] **Step 6: Commit**

```bash
git add ts/fastdb4ts/src/schema.ts tests/ts/test_schema_cache.mjs
git commit -m "perf(ts): dual-layer schema cache in getClassSchema (OPT-TS-3)

Add class-level __fdb_schema__ attribute as fast path before WeakMap.
Cold path writes the attribute via Object.defineProperty (non-enumerable,
non-writable). Falls back silently for sealed/frozen classes.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

<!-- END_TASK_4 -->

## Task 5: OPT-TS-4 — `Feature.readAllScalars()` / `writeAllScalars()` 批量字段 API（移植 Python o7）

**Files:**
- Modify: `ts/fastdb4ts/src/schema.ts`（`ClassSchema` 新增 `numericFieldCount`）
- Modify: `ts/fastdb4ts/src/feature.ts`（新增两个公开方法）
- Modify: `ts/fastdb4ts/src/index.ts`（无需改动，方法在 Feature 上）
- Test: `tests/ts/test_batch_fields.mjs`（新建）

**前提**：WASM 二进制必须包含 `getFieldsIntoHeap` / `setFieldsFromHeap`（C++ `getFieldsAsDoubles`/`setFieldsFromDoubles` 在 o7 中添加，embind 已绑定）。如当前 `.wasm` 文件未包含这些符号，需先执行 `bash ts/build-wasm.sh` 重新构建。

**验证 WASM 二进制是否有效**（在 Step 1 之前手动确认）：
```bash
# 方法：看 wasm 是否导出这两个函数
node -e "
import('../../ts/fastdb4ts/dist/index.js').then(async ({initFastdb,ORM,TableDefn,Feature,defineSchema,F64}) => {
  await initFastdb();
  const orm = ORM.truncate([new TableDefn(class P extends Feature { static schema = defineSchema({x:F64}) }, 1)]);
  const pt = orm.table(class P extends Feature { static schema = defineSchema({x:F64}) }).get(0);
  console.log('getFieldsIntoHeap:', typeof pt._origin?.getFieldsIntoHeap);
  console.log('setFieldsFromHeap:', typeof pt._origin?.setFieldsFromHeap);
})" 2>&1 || echo "需要先重建 WASM: bash ts/build-wasm.sh"
```

- [ ] **Step 1: 在 `schema.ts` 的 `ClassSchema` 中添加 `heapFieldIds` 字段**

在 `ClassSchema` 接口和 `getClassSchema()` 冷路径中添加：

接口（`schema.ts` 第 27 行附近）：
```ts
export interface ClassSchema {
  readonly fieldList: readonly SchemaFieldDefinition[];
  readonly fieldMap: ReadonlyMap<string, SchemaFieldDefinition>;
  readonly scalarFieldIds: readonly number[];
  readonly numericFieldIds: readonly number[];
  readonly refFieldIds: readonly number[];
  readonly listFieldIds: readonly number[];
  // Pre-allocated buffer for batch field access (getFieldsIntoHeap / setFieldsFromHeap).
  // Float64Array of length numericFieldIds.length, wrapping a shared ArrayBuffer.
  // Layout: [fieldId0, fieldId1, ...] as Uint32 values stored in Float64Array slots.
  readonly numericFieldCount: number;
}
```

在冷路径 schema 对象构造中添加 `numericFieldCount: numericFieldIds.length`。

- [ ] **Step 2: 在 `feature.ts` 添加辅助函数 `allocSchemaFieldIdsBuffer()`**

在 `feature.ts` 模块顶层（或 `schema.ts` 中）新增按需分配 WASM 堆 field_ids buffer 的辅助：

```ts
// Per-schema reusable WASM heap buffer for numeric field IDs.
// Allocated once per schema, freed only when the module is unloaded.
const schemaFieldIdBuffers = new WeakMap<ClassSchema, { ptr: number; nFields: number }>();

function getOrAllocFieldIdBuffer(
  schema: ClassSchema,
  module: FastdbModule
): { ptr: number; nFields: number } {
  const existing = schemaFieldIdBuffers.get(schema);
  if (existing) return existing;

  const ids = schema.numericFieldIds;
  const ptr = module._malloc(ids.length * 4); // u32 array
  const view = new DataView(module.HEAPU8.buffer);
  for (let i = 0; i < ids.length; i++) {
    view.setUint32(ptr + i * 4, ids[i], true);
  }
  const entry = { ptr, nFields: ids.length };
  schemaFieldIdBuffers.set(schema, entry);
  return entry;
}
```

- [ ] **Step 3: 在 `feature.ts` 的 `Feature` 类上添加两个公开方法**

```ts
/**
 * Batch-read all numeric scalar fields into a Float64Array (1 WASM call).
 * `out` must be pre-allocated with length >= numericFieldIds.length.
 * Returns the same `out` array (or a newly allocated one if omitted).
 * Only valid for db-mapped features (feature.fixed === true).
 */
readAllScalars(out?: Float64Array): Float64Array {
  if (!this.fixed || this._origin === null) {
    throw new FastdbRuntimeError('readAllScalars() requires a db-mapped feature (fixed === true).');
  }
  const db = this._db as { _module?: FastdbModule } & FeatureDatabaseHandle;
  // Retrieve module from wasm-loader singleton
  const module = getInitializedFastdbModule();
  const schema = this._schema;
  const { ptr: idsPtr, nFields } = getOrAllocFieldIdBuffer(schema, module);
  const outBuf = out ?? new Float64Array(nFields);
  const outPtr = module._malloc(nFields * 8);
  try {
    this._origin.getFieldsIntoHeap(idsPtr, nFields, outPtr);
    const dv = new DataView(module.HEAPU8.buffer);
    for (let i = 0; i < nFields; i++) {
      outBuf[i] = dv.getFloat64(outPtr + i * 8, true);
    }
  } finally {
    module._free(outPtr);
  }
  return outBuf;
}

/**
 * Batch-write all numeric scalar fields from a Float64Array (1 WASM call).
 * `values` length must equal numericFieldIds.length.
 * Only valid for db-mapped features (feature.fixed === true).
 */
writeAllScalars(values: Float64Array): void {
  if (!this.fixed || this._origin === null) {
    throw new FastdbRuntimeError('writeAllScalars() requires a db-mapped feature (fixed === true).');
  }
  const module = getInitializedFastdbModule();
  const schema = this._schema;
  const { ptr: idsPtr, nFields } = getOrAllocFieldIdBuffer(schema, module);
  if (values.length < nFields) {
    throw new FastdbUsageError(`writeAllScalars() expects ${nFields} values, got ${values.length}.`);
  }
  const valPtr = module._malloc(nFields * 8);
  try {
    const dv = new DataView(module.HEAPU8.buffer);
    for (let i = 0; i < nFields; i++) {
      dv.setFloat64(valPtr + i * 8, values[i], true);
    }
    this._origin.setFieldsFromHeap(idsPtr, valPtr, nFields);
  } finally {
    module._free(valPtr);
  }
}
```

> **注意**：`getInitializedFastdbModule` 已在 `wasm-loader.ts` 中导出，需在 `feature.ts` 中 import。

- [ ] **Step 4: 新建 `tests/ts/test_batch_fields.mjs`**

```js
import assert from 'node:assert/strict';
import test from 'node:test';
import {
  F64, U32, Feature, ORM, TableDefn, defineSchema, initFastdb,
} from '../../ts/fastdb4ts/dist/index.js';

await initFastdb();

class Vec3 extends Feature {
  static schema = defineSchema({ x: F64, y: F64, z: F64 });
}

test('readAllScalars returns correct values for db-mapped feature', () => {
  const orm = ORM.truncate([new TableDefn(Vec3, 1)]);
  const tbl = orm.table(Vec3);
  const pt = tbl.get(0);
  pt.x = 1.5; pt.y = 2.5; pt.z = 3.5;

  const out = pt.readAllScalars();
  assert.equal(out.length, 3);
  // Values correspond to numericFieldIds order (x=0, y=1, z=2)
  assert.equal(out[0], 1.5);
  assert.equal(out[1], 2.5);
  assert.equal(out[2], 3.5);
});

test('writeAllScalars sets all numeric fields in 1 call', () => {
  const orm = ORM.truncate([new TableDefn(Vec3, 1)]);
  const tbl = orm.table(Vec3);
  const pt = tbl.get(0);

  pt.writeAllScalars(new Float64Array([10.0, 20.0, 30.0]));
  assert.equal(pt.x, 10.0);
  assert.equal(pt.y, 20.0);
  assert.equal(pt.z, 30.0);
});

test('readAllScalars with pre-allocated out buffer', () => {
  const orm = ORM.truncate([new TableDefn(Vec3, 1)]);
  const pt = orm.table(Vec3).get(0);
  pt.x = 7.7; pt.y = 8.8; pt.z = 9.9;

  const out = new Float64Array(3);
  const returned = pt.readAllScalars(out);
  assert.strictEqual(returned, out, 'should return same buffer');
  assert.ok(Math.abs(out[0] - 7.7) < 1e-9);
});

test('readAllScalars throws for pure-python feature', () => {
  const pt = new Vec3({ x: 1, y: 2, z: 3 });
  assert.throws(() => pt.readAllScalars(), /db-mapped/);
});
```

- [ ] **Step 5: 运行测试**

```bash
npm --prefix ts/fastdb4ts run build && node --test tests/ts/test_batch_fields.mjs
```
Expected: 全部 PASS（若 WASM 不含批量 API，则此处报错，需先 `bash ts/build-wasm.sh`）

- [ ] **Step 6: 运行 bench，对比批量 vs 逐字段**

在 `bench_orm.mjs` 末尾添加对比项（运行前手动追加）：
```js
const ptBatch = ormBig.table(Point).get(0);
const reuseOut = new Float64Array(3);
bench('readAllScalars (3×F64, pre-alloc out)', () => { ptBatch.readAllScalars(reuseOut); });
bench('scalar_read_db_mapped ×3 (baseline)', () => { ptBatch.x; ptBatch.y; ptBatch.z; });
```

- [ ] **Step 7: Commit**

```bash
git add ts/fastdb4ts/src/schema.ts ts/fastdb4ts/src/feature.ts tests/ts/test_batch_fields.mjs
git commit -m "perf(ts): add Feature.readAllScalars/writeAllScalars batch field API (OPT-TS-4)

Port of Python o7: batch read/write all numeric scalar fields in 1 WASM
call via getFieldsIntoHeap / setFieldsFromHeap. Pre-allocates a per-schema
WASM heap buffer for field IDs (allocated once, reused across calls).

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

<!-- END_TASK_5 -->

## Task 6: OPT-TS-5 — `ORM.push()` 纯数值 Feature 批量写入

**Files:**
- Modify: `ts/fastdb4ts/src/orm.ts`
- Test: `tests/ts/test_column_way.mjs`（追加 push 测试）

**问题**：`push(feature)` 内循环对每个字段调用 `origin.setFieldDouble(idx, v)` 或 `origin.setFieldInt(idx, v)` = **N_fields 次 WASM 调用/feature**。对于纯数值 Feature（无 str/wstr/bytes/ref），可把所有数值字段打包到一次 `setFieldsFromHeap` 调用（1 次 WASM 调用/feature）。

**方案**：
1. 执行 `addFeatureBegin()` 之后、`addFeatureEnd()` 之前，如果 `schema.numericFieldIds.length === schema.fieldList.length`（纯数值 feature），走批量路径；否则走原有逐字段路径。
2. 批量路径：分配临时 WASM heap buffer（`values_ptr`），写入所有数值，调用 `origin.setFieldsFromHeap(...)`，释放 buffer。

- [ ] **Step 1: 补充 push 正确性测试**

在 `test_column_way.mjs` 添加：
```js
test('ORM.push numeric feature round-trip', () => {
  const o = ORM.create();
  for (let i = 0; i < 5; i++) {
    o.push(new Point({ x: i * 1.0, y: i * 2.0, z: i * 3.0 }));
  }
  o.combine();
  const tbl = o.table(Point);
  assert.equal(tbl.length, 5);
  assert.equal(tbl.get(2).x, 2.0);
  assert.equal(tbl.get(4).z, 12.0);
});
```

- [ ] **Step 2: 运行，确认通过（当前实现正确性基线）**

```bash
npm --prefix ts/fastdb4ts run test:ts 2>&1 | grep "ORM.push numeric"
```

- [ ] **Step 3: 修改 `orm.ts` — push() 增加批量数值路径**

在 `push()` 方法内，将原来的字段循环替换为：
```ts
push<T extends Feature>(feature: T, tableName?: string): void {
  // ... (existing table setup unchanged) ...

  origin.addFeatureBegin();
  try {
    const schema = getClassSchema(featureType);
    const numIds = schema.numericFieldIds;
    const allNumeric = numIds.length === schema.fieldList.length;

    if (allNumeric && numIds.length > 0) {
      // Batch numeric path: 1 WASM call instead of N_fields calls
      const nFields = numIds.length;
      const idsPtr = this.module._malloc(nFields * 4);
      const valPtr = this.module._malloc(nFields * 8);
      try {
        const dv = new DataView(this.module.HEAPU8.buffer);
        // Write field IDs (Uint32)
        for (let i = 0; i < nFields; i++) {
          dv.setUint32(idsPtr + i * 4, numIds[i], true);
        }
        // Write field values (Float64)
        for (let i = 0; i < nFields; i++) {
          const fld = schema.fieldList[numIds[i]];
          dv.setFloat64(valPtr + i * 8, Number((feature as Record<string, unknown>)[fld.name] ?? 0), true);
        }
        (origin as WxLayerTableBuildHandle & { setFieldsFromHeap?: (a: number, b: number, c: number) => void })
          .setFieldsFromHeap?.(idsPtr, valPtr, nFields);
      } finally {
        this.module._free(idsPtr);
        this.module._free(valPtr);
      }
    } else {
      // Mixed / string path: original per-field loop
      for (const field of schema.fieldList) {
        const value = (feature as Record<string, unknown>)[field.name];
        const kind = field.entry.kind;
        if (kind === 'bool') {
          origin.setFieldInt(field.index, value ? 1 : 0);
        } else if (kind === 'u8' || kind === 'u16' || kind === 'u32' || kind === 'i32') {
          origin.setFieldInt(field.index, Math.trunc(Number(value)));
        } else if (kind === 'u8n' || kind === 'u16n' || kind === 'f32' || kind === 'f64') {
          origin.setFieldDouble(field.index, Number(value));
        } else if (kind === 'str' || kind === 'wstr') {
          origin.setFieldString(field.index, String(value ?? ''));
        } else {
          throw new FastdbUsageError(`push() does not support field kind "${kind}" yet.`);
        }
      }
    }
  } finally {
    origin.addFeatureEnd();
  }
}
```

> **注意**：`WxLayerTableBuildHandle` 目前未声明 `setFieldsFromHeap`。若 embind build 包含此方法，需在 `wasm-loader.ts` 的接口中补充声明；若 build layer 不支持，考虑改走 Feature 的 `_origin` 路径（即先 begin，得到 feature handle 再批量写）。如果 `WxLayerTableBuildHandle` 不提供 `setFieldsFromHeap`，此任务退化为可选项（`ORM.push` 优化跳过，仍走原逐字段路径）。

- [ ] **Step 4: 确认 WxLayerTableBuildHandle 是否有 setFieldsFromHeap**

```bash
grep -n "setFieldsFromHeap" ts/embind/fastdb4ts.cpp ts/fastdb4ts/src/wasm-loader.ts
```

如果 `WxLayerTableBuildHandle` 没有此方法：
- 在 `wasm-loader.ts` 的 `WxLayerTableBuildHandle` 接口添加可选声明：`setFieldsFromHeap?(idsPtr: number, valuesPtr: number, nFields: number): void;`
- 若 embind 未绑定，此优化在不重建 WASM 的情况下不可用，跳至 Step 7

- [ ] **Step 5: 运行测试，确认通过**

```bash
npm --prefix ts/fastdb4ts run test:ts
```

- [ ] **Step 6: 运行 bench 对比 push 路径**

```bash
npm --prefix ts/fastdb4ts run bench:orm 2>&1 | grep "push"
```

- [ ] **Step 7: Commit**

```bash
git add ts/fastdb4ts/src/orm.ts ts/fastdb4ts/src/wasm-loader.ts tests/ts/test_column_way.mjs
git commit -m "perf(ts): ORM.push() batch numeric write via setFieldsFromHeap (OPT-TS-5)

For all-numeric Feature schemas, accumulate field values and call
setFieldsFromHeap once per feature instead of N_fields separate
setFieldDouble/setFieldInt WASM calls. Mixed schemas fall back to the
original per-field loop.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

<!-- END_TASK_6 -->

## Task 7: CHANGELOG 更新

**Files:**
- Modify: `CHANGELOG.md`

- [ ] **Step 1: 在 `<!-- BEGIN:fastdb4ts -->` ... `<!-- END:fastdb4ts -->` 中添加 Performance 条目**

```markdown
### Performance
- `Table.column.x` 重复访问改为缓存 `StridedColumn` 实例，消除每次构造的 3 次 WASM 调用
- `StridedColumn.set()` / `fill()` 改用 DataView 直写 WASM 线性内存，`fill(N=1000)` 从 2000 次 WASM 调用降至 0
- `getClassSchema()` 添加 class 属性快速缓存路径（`__fdb_schema__`），避免 WeakMap 查找
- `Feature.readAllScalars()` / `writeAllScalars()` 批量字段 API，M 个数值字段合并为 1 次 WASM 调用
- `ORM.push()` 纯数值 Feature 路径使用 `setFieldsFromHeap` 批写，N_fields WASM 调用降至 1
```

- [ ] **Step 2: Commit**

```bash
git add CHANGELOG.md
git commit -m "docs: update CHANGELOG with fastdb4ts ORM performance improvements

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## 执行优先级建议

| 优先级 | 任务 | 需要 WASM 重建 | 预期收益 |
|---|---|---|---|
| P0 | Task 1（bench） | 否 | 量化基线，后续必须 |
| P1 | Task 3（StridedColumn fill/set） | 否 | **最大收益**：fill N=1000 ≈ 0 WASM 调用 vs 2000 |
| P1 | Task 2（column 缓存） | 否 | 每次 `table.column.x` 省 3 WASM 调用 |
| P2 | Task 4（schema 双层缓存） | 否 | feature_init 微小改善 |
| P2 | Task 5（readAllScalars）| **是（若 WASM 未含 o7 批量 API）** | 3×scalar_read → 1 WASM 调用 |
| P3 | Task 6（push 批写） | 视情况 | push N=100 改善约 3× |
| last | Task 7（CHANGELOG） | 否 | 记录 |
