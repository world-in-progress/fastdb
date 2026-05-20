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


def test_fastdb4py_codegen_import_does_not_expose_c_two_codegen():
    script = """
import json
import fastdb4py.codegen as codegen

forbidden_attrs = [
    'CTwoCodegenError',
    'generate_c_two_typescript_helpers',
    'run_codegen_c_two_ts',
]
print(json.dumps([name for name in forbidden_attrs if hasattr(codegen, name)]))
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


def test_fdb_codegen_help_does_not_advertise_c_two_typescript_target():
    completed = subprocess.run(
        [sys.executable, '-m', 'fastdb4py.cli', 'codegen', '--help'],
        check=False,
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert '--c-two-ts' not in completed.stdout
    assert 'C-Two' not in completed.stdout
