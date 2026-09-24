import subprocess
import sys


def test_fastdb4py_imports_in_fresh_python_process():
    completed = subprocess.run(
        [sys.executable, '-c', 'import fastdb4py'],
        check=False,
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr


def test_fastdb4py_import_does_not_expose_c_two_surfaces():
    script = """
import json
import sys
import fastdb4py

forbidden_attrs = [
    'CTwoFastdbCodecProvider',
    'derive_c_two_bridge',
    'install_c_two_provider',
    'c_two_bridge',
]
loaded_modules = [
    name
    for name in sorted(sys.modules)
    if name.startswith('fastdb4py.c_two') or name == 'fastdb4py.codegen.c_two_ts'
]
print(json.dumps({
    'attrs': [name for name in forbidden_attrs if hasattr(fastdb4py, name)],
    'loaded_modules': loaded_modules,
}))
"""
    completed = subprocess.run(
        [sys.executable, '-c', script],
        check=False,
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.strip() == '{"attrs": [], "loaded_modules": []}'


def test_fastdb4py_c_two_glue_modules_are_not_package_surfaces():
    script = """
import importlib
import json

results = {}
for module_name in [
    'fastdb4py.c_two_provider',
    'fastdb4py.c_two_bridge',
    'fastdb4py.c_two_call',
    'fastdb4py.codegen.c_two_ts',
]:
    try:
        importlib.import_module(module_name)
    except ModuleNotFoundError:
        results[module_name] = 'missing'
    else:
        results[module_name] = 'present'
print(json.dumps(results, sort_keys=True))
"""
    completed = subprocess.run(
        [sys.executable, '-c', script],
        check=False,
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.strip() == (
        '{"fastdb4py.c_two_bridge": "missing", '
        '"fastdb4py.c_two_call": "missing", '
        '"fastdb4py.c_two_provider": "missing", '
        '"fastdb4py.codegen.c_two_ts": "missing"}'
    )


def test_fastdb4py_codegen_package_is_not_a_package_surface():
    script = """
import json
import importlib

try:
    importlib.import_module('fastdb4py.codegen')
except ModuleNotFoundError:
    result = 'missing'
else:
    result = 'present'
print(json.dumps(result))
"""
    completed = subprocess.run(
        [sys.executable, '-c', script],
        check=False,
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.strip() == '"missing"'


def test_fdb_codegen_help_advertises_only_core_artifact_targets():
    completed = subprocess.run(
        [sys.executable, '-m', 'fastdb4py.cli', 'codegen', '--help'],
        check=False,
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert '--target {cpp,rust,python,typescript}' in completed.stdout
    assert '--output OUTPUT' in completed.stdout
    assert '--ts' not in completed.stdout
    assert '--c-' + 'two-ts' not in completed.stdout
    assert 'C-' + 'Two' not in completed.stdout
    assert 'feature' not in completed.stdout.lower()


def test_package_root_import_loads_neither_payload_nor_removed_authority():
    script = """
import json
import sys
import fastdb4py

removed_rpc_suffix = 'call' + '_' + 'db'
forbidden_modules = [
    'fastdb4py.payload',
    'fastdb4py.' + removed_rpc_suffix,
    'fastdb4py.schema',
    'fastdb4py.require',
    'fastdb4py.allocator',
    'fastdb4py.codegen',
]
print(json.dumps([
    name for name in forbidden_modules if name in sys.modules
]))
"""
    completed = subprocess.run(
        [sys.executable, '-c', script],
        check=False,
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.strip() == '[]'
