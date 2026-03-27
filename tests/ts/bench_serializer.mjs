/**
 * Benchmark: FastSerializer dumps/loads for a complex Feature.
 *
 * Mirrors the Python bench_fastser_buf.py — uses a PointCloud-like Feature
 * with scalars, strings, typed numeric lists, and mixed payloads.
 *
 * Output: single line "METRIC=<value>" where value = geometric mean of
 * dumps+loads times across all test sizes (in µs). Lower is better.
 */
import {
  F64,
  FastSerializer,
  Feature,
  I32,
  STR,
  U32,
  defineSchema,
  initFastdb,
  listOf,
} from '../../ts/fastdb4ts/dist/index.js';

await initFastdb();

// --- Complex Feature definition ---
class PointCloud extends Feature {
  static schema = defineSchema({
    name: STR,
    id: U32,
    timestamp: F64,
    quality: F64,
    positions: listOf(F64),   // 3N floats
    indices: listOf(U32),     // triangle indices
    labels: listOf(STR),      // string labels (non-numeric, in blob)
  });
}

// --- Helpers ---
function makeCloud(n) {
  const positions = [];
  for (let i = 0; i < n * 3; i++) positions.push(i * 0.01);
  const indices = [];
  for (let i = 0; i < n * 3; i++) indices.push(i % n);
  const labels = [];
  for (let i = 0; i < Math.min(n, 20); i++) labels.push(`v${i}`);
  return new PointCloud({
    name: `cloud_${n}`,
    id: n,
    timestamp: 1234567890.123,
    quality: 0.95,
    positions,
    indices,
    labels,
  });
}

function bench(fn, warmup = 5, repeat = 30) {
  for (let i = 0; i < warmup; i++) fn();
  const times = [];
  for (let i = 0; i < repeat; i++) {
    const t0 = performance.now();
    fn();
    const t1 = performance.now();
    times.push((t1 - t0) * 1000); // ms → µs
  }
  times.sort((a, b) => a - b);
  return times[Math.floor(times.length / 2)]; // median
}

// --- Benchmark ---
const SIZES = [10, 100, 1000, 10_000];
const results = {};

console.log('='.repeat(72));
console.log('  FastSerializer TS Benchmark — Complex PointCloud Feature');
console.log('='.repeat(72));

for (const n of SIZES) {
  const obj = makeCloud(n);
  const blob = FastSerializer.dumps(obj);
  const tDumps = bench(() => FastSerializer.dumps(obj));
  const tLoads = bench(() => FastSerializer.loads(blob, PointCloud));

  results[`dumps_${n}`] = tDumps;
  results[`loads_${n}`] = tLoads;

  console.log(`\n  N=${String(n).padStart(6)} vertices  (${blob.length} B)`);
  console.log(`    dumps: ${tDumps.toFixed(1).padStart(10)} µs`);
  console.log(`    loads: ${tLoads.toFixed(1).padStart(10)} µs`);
}

const allTimes = Object.values(results);
const geoMean = Math.exp(allTimes.reduce((s, t) => s + Math.log(t), 0) / allTimes.length);

console.log(`\n${'='.repeat(72)}`);
console.log(`  Geometric mean: ${geoMean.toFixed(2)} µs`);
console.log(`${'='.repeat(72)}`);
console.log(`METRIC=${geoMean.toFixed(2)}`);
