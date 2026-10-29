"""Measure current process memory after complete standalone database cycles.

Runs in its own process so allocator warm-up and the import baseline are explicit.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import sys

import numpy as np
import fastdb4py as fdb

# ctypes libraries cache function objects that reference their library. Build
# these once so the monitor itself does not add cycles with GC disabled.
if sys.platform == 'darwin':
    _proc_pidinfo = ctypes.CDLL('/usr/lib/libproc.dylib').proc_pidinfo
    _proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
elif sys.platform == 'win32':
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('faults', wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in (
                'peak_working', 'working', 'peak_paged', 'paged',
                'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile', 'private',
            )
        ]

    _kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    _kernel.GetCurrentProcess.restype = wintypes.HANDLE
    _psapi = ctypes.WinDLL('psapi', use_last_error=True)
    _psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]


@fdb.feature
class MemoryValue:
    value: fdb.F32


def resident_bytes():
    if sys.platform == 'darwin':
        info = ctypes.create_string_buffer(96)
        count = _proc_pidinfo(os.getpid(), 4, 0, info, len(info))
        if count != len(info):
            raise OSError('proc_pidinfo(PROC_PIDTASKINFO) failed')
        return ctypes.c_uint64.from_buffer(info, 8).value
    if sys.platform == 'win32':
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        if not _psapi.GetProcessMemoryInfo(_kernel.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        return counters.private
    return int(Path('/proc/self/statm').read_text().split()[1]) * os.sysconf('SC_PAGE_SIZE')


def run(path, rows, cycles, mode):
    import gc
    gc.disable()
    values = np.random.default_rng(0).random(rows, dtype=np.float32)
    if mode == 'read':
        engine = fdb.RecordEngine.truncate([fdb.Layout(MemoryValue, rows)])
        engine.table(MemoryValue).fill(value=values)
        engine.save(str(path))
        engine.close()
        del engine
    results = []
    for iteration in range(cycles):
        steps = [('start', resident_bytes())]
        if mode == 'write':
            engine = fdb.RecordEngine.truncate([fdb.Layout(MemoryValue, rows)])
            steps.append(('truncate', resident_bytes()))
            table = engine.table(MemoryValue)
            table.fill(value=values)
            steps.append(('fill', resident_bytes()))
            engine.save(str(path))
            steps.append(('save', resident_bytes()))
            del table
        else:
            engine = fdb.RecordEngine.load(str(path), from_file=True)
            steps.append(('load', resident_bytes()))
            copied = np.array(engine.table(MemoryValue).column.value, copy=True)
            np.testing.assert_array_equal(copied, values)
            steps.append(('column_copy', resident_bytes()))
            del copied
        owns = engine._origin.thisown
        engine.close()
        steps.append(('close', resident_bytes()))
        del engine
        steps.append(('released', resident_bytes()))
        entry = {'iteration': iteration + 1, 'thisown': owns, 'bytes': dict(steps)}
        entry['net_bytes'] = steps[-1][1] - steps[0][1]
        results.append(entry)
        print(json.dumps(entry), flush=True)
    summary = {'mode': mode, 'rows': rows, 'cycles': cycles,
               'retained_bytes': [r['bytes']['released'] for r in results]}
    print(json.dumps(summary), flush=True)
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--path', type=Path, required=True)
    parser.add_argument('--rows', type=int, default=1_000_000)
    parser.add_argument('--cycles', type=int, default=8)
    parser.add_argument('--mode', choices=('write', 'read'), required=True)
    args = parser.parse_args()
    run(args.path, args.rows, args.cycles, args.mode)
