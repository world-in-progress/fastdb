import argparse
import gc
import multiprocessing as mp
import pickle
import statistics
import time
import traceback
from dataclasses import dataclass
from typing import List

from fastdb4py import FastSerializer, Feature, U32, F64, I32, STR


class Point(Feature):
    x: F64
    y: F64


class NumericPayload(Feature):
    ids: List[U32]
    values: List[F64]


class MixedPayload(Feature):
    name: STR
    score: F64
    tags: List[str]
    points: List[Point]


class RecursiveNode(Feature):
    val: I32
    next: 'RecursiveNode'


@dataclass
class PPoint:
    x: float
    y: float


@dataclass
class PNumericPayload:
    ids: List[int]
    values: List[float]


@dataclass
class PMixedPayload:
    name: str
    score: float
    tags: List[str]
    points: List[PPoint]


class PRecursiveNode:
    def __init__(self, val: int):
        self.val = val
        self.next = None


def _timeit(func, iterations: int, warmup: int) -> list[float]:
    for _ in range(warmup):
        func()

    samples = []
    for _ in range(iterations):
        t0 = time.perf_counter()
        func()
        t1 = time.perf_counter()
        samples.append(t1 - t0)

    return samples


def _summary(samples: list[float]) -> tuple[float, float]:
    mean_us = statistics.mean(samples) * 1_000_000
    p50_us = statistics.median(samples) * 1_000_000
    return mean_us, p50_us


def _build_numeric_payload_pair(n: int):
    ids = list(range(n))
    values = [float(i) * 0.125 for i in range(n)]

    fast_obj = NumericPayload(ids=ids, values=values)
    pickle_obj = PNumericPayload(ids=ids, values=values)
    return fast_obj, pickle_obj


def _build_mixed_payload_pair(n: int):
    points = [Point(x=float(i), y=float(i) * 0.5) for i in range(n)]
    ppoints = [PPoint(x=float(i), y=float(i) * 0.5) for i in range(n)]
    tags = [f"tag_{i}" for i in range(64)] + ["你好", "emoji🙂", "line1\nline2"]
    fast_obj = MixedPayload(
        name="mixed_payload",
        score=123.456,
        tags=tags,
        points=points,
    )

    pickle_obj = PMixedPayload(
        name="mixed_payload",
        score=123.456,
        tags=tags,
        points=ppoints,
    )

    return fast_obj, pickle_obj


def _build_recursive_payload_pair(n: int):
    f_head = RecursiveNode(val=0)
    f_current = f_head

    p_head = PRecursiveNode(val=0)
    p_current = p_head

    for i in range(1, n):
        f_nxt = RecursiveNode(val=i)
        f_current.next = f_nxt
        f_current = f_nxt

        p_nxt = PRecursiveNode(val=i)
        p_current.next = p_nxt
        p_current = p_nxt

    f_current.next = f_head
    p_current.next = p_head
    return f_head, p_head


def benchmark_case(name: str, fast_obj, root_type, pickle_obj, iterations: int, warmup: int):
    gc.collect()

    fast_bytes = FastSerializer.dumps(fast_obj)
    pickle_bytes = pickle.dumps(pickle_obj, protocol=pickle.HIGHEST_PROTOCOL)

    fast_dump_samples = _timeit(lambda: FastSerializer.dumps(fast_obj), iterations, warmup)
    fast_load_samples = _timeit(lambda: FastSerializer.loads(fast_bytes, root_type), iterations, warmup)

    pickle_dump_samples = _timeit(
        lambda: pickle.dumps(pickle_obj, protocol=pickle.HIGHEST_PROTOCOL),
        iterations,
        warmup,
    )
    pickle_load_samples = _timeit(lambda: pickle.loads(pickle_bytes), iterations, warmup)

    fast_dump_mean, fast_dump_p50 = _summary(fast_dump_samples)
    fast_load_mean, fast_load_p50 = _summary(fast_load_samples)

    pickle_dump_mean, pickle_dump_p50 = _summary(pickle_dump_samples)
    pickle_load_mean, pickle_load_p50 = _summary(pickle_load_samples)

    return {
        "name": name,
        "fast_size": len(fast_bytes),
        "pickle_size": len(pickle_bytes),
        "fast_dump_mean": fast_dump_mean,
        "fast_dump_p50": fast_dump_p50,
        "fast_load_mean": fast_load_mean,
        "fast_load_p50": fast_load_p50,
        "pickle_dump_mean": pickle_dump_mean,
        "pickle_dump_p50": pickle_dump_p50,
        "pickle_load_mean": pickle_load_mean,
        "pickle_load_p50": pickle_load_p50,
    }


def _build_case(case_name: str, numeric_size: int, mixed_points: int, cyclic_nodes: int):
    if case_name == "numeric":
        fast_obj, pickle_obj = _build_numeric_payload_pair(numeric_size)
        return fast_obj, NumericPayload, pickle_obj
    if case_name == "mixed":
        fast_obj, pickle_obj = _build_mixed_payload_pair(mixed_points)
        return fast_obj, MixedPayload, pickle_obj
    if case_name == "cyclic":
        fast_obj, pickle_obj = _build_recursive_payload_pair(cyclic_nodes)
        return fast_obj, RecursiveNode, pickle_obj
    raise ValueError(f"Unknown case: {case_name}")


def _worker(queue: mp.Queue, case_name: str, iterations: int, warmup: int, numeric_size: int, mixed_points: int, cyclic_nodes: int):
    try:
        fast_obj, root_type, pickle_obj = _build_case(case_name, numeric_size, mixed_points, cyclic_nodes)
        result = benchmark_case(case_name, fast_obj, root_type, pickle_obj, iterations, warmup)
        queue.put({"ok": True, "result": result})
    except Exception as exc:
        queue.put({"ok": False, "error": repr(exc), "traceback": traceback.format_exc()})


def run_case_isolated(case_name: str, iterations: int, warmup: int, numeric_size: int, mixed_points: int, cyclic_nodes: int, timeout: int):
    queue: mp.Queue = mp.Queue()
    proc = mp.Process(
        target=_worker,
        args=(queue, case_name, iterations, warmup, numeric_size, mixed_points, cyclic_nodes),
    )
    proc.start()
    proc.join(timeout=timeout)

    if proc.is_alive():
        proc.terminate()
        proc.join()
        return {
            "name": case_name,
            "status": "timeout",
        }

    if proc.exitcode != 0:
        return {
            "name": case_name,
            "status": f"crash({proc.exitcode})",
        }

    if queue.empty():
        return {
            "name": case_name,
            "status": "no-result",
        }

    msg = queue.get()
    if not msg.get("ok"):
        return {
            "name": case_name,
            "status": f"error({msg.get('error')})",
            "traceback": msg.get("traceback", ""),
        }

    out = msg["result"]
    out["status"] = "ok"
    return out


def print_report(rows: list[dict], iterations: int, warmup: int):
    print("FastSerializer vs pickle benchmark")
    print(f"iterations={iterations}, warmup={warmup}\n")

    header = (
        f"{'Case':<14} | {'Status':<12} | {'Size(Fast)':>10} | {'Size(Pickle)':>12} | "
        f"{'Dump Fast μs':>12} | {'Dump Pickle μs':>14} | "
        f"{'Load Fast μs':>12} | {'Load Pickle μs':>14}"
    )
    print(header)
    print("-" * len(header))

    for row in rows:
        if row.get("status") != "ok":
            print(
                f"{row['name']:<14} | {row.get('status', 'unknown'):<12} | {'-':>10} | {'-':>12} | {'-':>12} | {'-':>14} | {'-':>12} | {'-':>14}"
            )
            continue

        print(
            f"{row['name']:<14} | {row.get('status', 'ok'):<12} | {row['fast_size']:>10} | {row['pickle_size']:>12} | "
            f"{row['fast_dump_p50']:>12.2f} | {row['pickle_dump_p50']:>14.2f} | "
            f"{row['fast_load_p50']:>12.2f} | {row['pickle_load_p50']:>14.2f}"
        )

    print("\nDetail (mean μs):")
    for row in rows:
        if row.get("status") != "ok":
            print(f"- {row['name']}: {row.get('status', 'unknown')}")
            tb = row.get("traceback", "")
            if tb:
                print(tb.rstrip())
            continue
        print(
            f"- {row['name']}: "
            f"dump fast={row['fast_dump_mean']:.2f}, pickle={row['pickle_dump_mean']:.2f}; "
            f"load fast={row['fast_load_mean']:.2f}, pickle={row['pickle_load_mean']:.2f}"
        )


def main():
    parser = argparse.ArgumentParser(description="Benchmark FastSerializer against pickle")
    parser.add_argument("--iterations", type=int, default=200)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--numeric-size", type=int, default=8, help="List length for numeric payload")
    parser.add_argument("--mixed-points", type=int, default=4, help="Point count for mixed payload")
    parser.add_argument("--cyclic-nodes", type=int, default=8, help="Node count for cyclic payload")
    parser.add_argument("--timeout", type=int, default=30, help="Per-case timeout in seconds")
    args = parser.parse_args()

    print("Note: Cases run in isolated subprocesses to tolerate native-level crashes.")
    print("      Increase sizes gradually with --numeric-size/--mixed-points/--cyclic-nodes for stress tests.\n")

    rows = []
    for case_name in ["numeric", "mixed", "cyclic"]:
        row = run_case_isolated(
            case_name=case_name,
            iterations=args.iterations,
            warmup=args.warmup,
            numeric_size=args.numeric_size,
            mixed_points=args.mixed_points,
            cyclic_nodes=args.cyclic_nodes,
            timeout=args.timeout,
        )
        rows.append(row)

    print_report(rows, args.iterations, args.warmup)


if __name__ == "__main__":
    main()
