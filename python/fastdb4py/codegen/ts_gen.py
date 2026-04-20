"""
TypeScript code generator for fastdb4py Feature classes.

Pipeline:
  Phase 1 - Discovery: scan .py files, dynamic-import, find Feature subclasses
  Phase 2 - Analysis:  build dep graph, topological sort, detect cycles
  Phase 3 - Generation: emit .ts per .py file
"""

import sys
import importlib.util
import inspect
import os
import types
from pathlib import Path
from collections import deque
from typing import get_type_hints, get_origin, get_args, Any, Dict, List, Set, Tuple, Optional, Type

# ─── Type imports from fastdb4py ──────────────────────────────────────────────

from fastdb4py.registry import is_feature
from fastdb4py.feature.base import BaseFeature
import fastdb4py as _fdb


def _is_feature_cls(cls: type) -> bool:
    """Check if *cls* is a feature class for codegen purposes.

    Matches both old-style ``Feature`` subclasses (via ``issubclass``) and
    new ``@feature``-decorated classes (via ``is_feature``).
    Excludes ``BaseFeature`` itself.
    """
    if not isinstance(cls, type):
        return False
    if cls is BaseFeature:
        return False
    return is_feature(cls) or issubclass(cls, BaseFeature)


class CodegenError(Exception):
    """Raised when codegen encounters an unresolvable type or configuration error."""

# ─── TypeVar → TS name map ────────────────────────────────────────────────────

_TYPEVAR_TO_TS: Dict[Any, str] = {
    _fdb.BOOL:  'BOOL',
    _fdb.U8:    'U8',
    _fdb.U16:   'U16',
    _fdb.U32:   'U32',
    _fdb.I32:   'I32',
    _fdb.U8N:   'U8N',
    _fdb.U16N:  'U16N',
    _fdb.F32:   'F32',
    _fdb.F64:   'F64',
    _fdb.STR:   'STR',
    _fdb.WSTR:  'WSTR',
    _fdb.REF:   'REF',
    _fdb.BYTES: 'BYTES',
}

_TYPEVAR_TS_DECL: Dict[Any, str] = {
    _fdb.BOOL:  'boolean',
    _fdb.U8:    'number',
    _fdb.U16:   'number',
    _fdb.U32:   'number',
    _fdb.I32:   'number',
    _fdb.U8N:   'number',
    _fdb.U16N:  'number',
    _fdb.F32:   'number',
    _fdb.F64:   'number',
    _fdb.STR:   'string',
    _fdb.WSTR:  'string',
    _fdb.REF:   'unknown',
    _fdb.BYTES: 'Uint8Array',
}

_NATIVE_TO_TS_SCHEMA: Dict[type, str] = {
    int:   'I32',
    float: 'F64',
    str:   'STR',
    bool:  'BOOL',
}

_NATIVE_TO_TS_DECL: Dict[type, str] = {
    int:   'number',
    float: 'number',
    str:   'string',
    bool:  'boolean',
}


def _get_hints_safe(cls: Type, ctx: 'CodegenContext') -> Dict[str, Any]:
    """Call get_type_hints with per-class scoped localns for forward ref resolution."""
    localns = ctx.resolve_ctx_for(cls)
    try:
        return get_type_hints(cls, localns=localns)
    except Exception:
        try:
            # Fall back: also include the class's own module globals
            import sys as _sys
            mod = _sys.modules.get(getattr(cls, '__module__', ''), None)
            globs = vars(mod) if mod is not None else {}
            return get_type_hints(cls, globalns=globs, localns=localns)
        except Exception:
            return {}


# ─── CodegenContext ───────────────────────────────────────────────────────────


class CodegenContext:
    """Replaces the flat class_registry with file-scoped resolution and canonicalization."""

    def __init__(
        self,
        file_to_classes: Dict[Path, List[Type]],
        class_to_file: Dict[Type, Path],
        input_dir: Path,
    ):
        self.file_to_classes = file_to_classes
        self.class_to_file = class_to_file
        self.input_dir = input_dir

        # Derived: name → list of classes with that name (across all files)
        self.name_to_classes: Dict[str, List[Type]] = {}
        for cls in class_to_file:
            self.name_to_classes.setdefault(cls.__name__, []).append(cls)

    @property
    def all_classes(self) -> List[Type]:
        return list(self.class_to_file.keys())

    def resolve_ctx_for(self, cls: Type) -> Dict[str, Type]:
        """Build a name→class dict scoped to what this class can see.

        Priority:
        1. Same-file classes (siblings)
        2. Module globals (imported classes)
        3. Globally unique names (convenience fallback)
        """
        ctx: Dict[str, Type] = {}

        # 1. Same-file classes
        src_file = self.class_to_file.get(cls)
        if src_file is not None:
            for sibling in self.file_to_classes.get(src_file, []):
                ctx[sibling.__name__] = sibling

        # 2. Module globals
        mod = sys.modules.get(getattr(cls, '__module__', ''), None)
        if mod is not None:
            for name, obj in vars(mod).items():
                if _is_feature_cls(obj):
                    if name not in ctx:
                        ctx[name] = obj

        # 3. Globally unique names
        for name, clss in self.name_to_classes.items():
            if name not in ctx and len(clss) == 1:
                ctx[name] = clss[0]

        return ctx

    def canonicalize(self, cls: Type) -> Optional[Type]:
        """Map an arbitrary Feature class ref to its canonical discovered counterpart.

        Handles the case where Python's import system created a separate class object
        (e.g. 'from geometry import Point' creates geometry.Point, not _fdb_codegen.geometry.Point).
        """
        # Direct match
        if cls in self.class_to_file:
            return cls

        # Match by source file + name via inspect.getfile
        try:
            src = Path(inspect.getfile(cls)).resolve()
            rel = src.relative_to(self.input_dir)
            for c in self.file_to_classes.get(rel, []):
                if c.__name__ == cls.__name__:
                    return c
        except (TypeError, OSError, ValueError):
            pass

        # Unambiguous name match
        matches = self.name_to_classes.get(cls.__name__, [])
        if len(matches) == 1:
            return matches[0]

        return None

    def get_file(self, cls: Type) -> Optional[Path]:
        return self.class_to_file.get(cls)


# ─── Phase 1: Discovery ───────────────────────────────────────────────────────


def scan_py_files(input_dir: Path) -> List[Path]:
    """Return all .py files under input_dir, excluding pure dunder filenames."""
    result = []
    for p in sorted(input_dir.rglob('*.py')):
        stem = p.stem
        if stem.startswith('__') and stem.endswith('__'):
            continue
        result.append(p)
    return result


def load_module(py_file: Path, input_dir: Path) -> Tuple[Optional[types.ModuleType], Optional[str]]:
    """Dynamically load a Python file as a module."""
    rel = py_file.relative_to(input_dir)
    # Build a unique module name from path parts
    parts = list(rel.with_suffix('').parts)
    module_name = '_fdb_codegen.' + '.'.join(parts)

    # Temporarily extend sys.path so intra-package imports resolve
    paths_to_add = []
    for p in [str(input_dir), str(py_file.parent)]:
        if p not in sys.path:
            paths_to_add.append(p)
            sys.path.insert(0, p)

    try:
        spec = importlib.util.spec_from_file_location(module_name, py_file)
        if spec is None or spec.loader is None:
            return None, f"Could not create spec for {py_file}"
        module = importlib.util.module_from_spec(spec)
        # Register in sys.modules so forward references in get_type_hints can resolve
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
        return module, None
    except SyntaxError as e:
        return None, f"SyntaxError in {py_file}: {e}"
    except ImportError as e:
        return None, f"ImportError in {py_file}: {e}"
    except Exception as e:
        return None, f"Error loading {py_file}: {type(e).__name__}: {e}"
    finally:
        for p in paths_to_add:
            try:
                sys.path.remove(p)
            except ValueError:
                pass


def discover_features(module: types.ModuleType, py_file: Path, input_dir: Path) -> List[Type]:
    """Return Feature subclasses defined in py_file (not merely imported)."""
    module_name = module.__name__
    found = []
    for _name, cls in inspect.getmembers(module, inspect.isclass):
        if not _is_feature_cls(cls):
            continue
        if cls.__name__ == 'Feature':
            continue
        # Only include classes defined in this module (not imported from elsewhere)
        if getattr(cls, '__module__', None) != module_name:
            continue
        found.append(cls)
    return found


def discover_all(input_dir: Path) -> Tuple[Dict[Path, List[Type]], Dict[Type, Path], List[str]]:
    """Discover all Feature subclasses across all .py files in input_dir.

    Returns:
        file_to_classes: rel_path -> [classes defined in that file]
        class_to_file:   class -> rel_path
        errors:          list of error strings
    """
    file_to_classes: Dict[Path, List[Type]] = {}
    class_to_file: Dict[Type, Path] = {}
    errors: List[str] = []

    for py_file in scan_py_files(input_dir):
        module, err = load_module(py_file, input_dir)
        if err:
            errors.append(err)
            continue

        rel = py_file.relative_to(input_dir)
        classes = discover_features(module, py_file, input_dir)
        if classes:
            file_to_classes[rel] = classes
        for cls in classes:
            class_to_file[cls] = rel

    return file_to_classes, class_to_file, errors


# ─── Phase 2: Analysis ────────────────────────────────────────────────────────


def _resolve_hint(hint: Any, resolve_ctx: Dict[str, Type]) -> Any:
    """Resolve a string forward reference to an actual type, if possible."""
    if isinstance(hint, str):
        return resolve_ctx.get(hint, hint)
    return hint


def build_dep_graph(cls: Type, ctx: CodegenContext) -> Set[Type]:
    """Return set of Feature classes cls directly depends on."""
    deps: Set[Type] = set()
    resolve_ctx = ctx.resolve_ctx_for(cls)
    try:
        hints = get_type_hints(cls, localns=resolve_ctx)
    except Exception:
        return deps

    for field, hint in hints.items():
        if field.startswith('_'):
            continue

        hint = _resolve_hint(hint, resolve_ctx)
        origin = get_origin(hint)

        # List[X]
        if origin is list:
            args = get_args(hint)
            if args:
                inner = _resolve_hint(args[0], resolve_ctx)
                if _is_feature_cls(inner):
                    deps.add(inner)
            continue

        # String forward ref that didn't resolve to a known class
        if isinstance(hint, str):
            if hint in resolve_ctx:
                deps.add(resolve_ctx[hint])
            else:
                raise CodegenError(
                    f"In class '{cls.__name__}', field '{field}' references undeclared type "
                    f"'{hint}'. Ensure it is defined in the same directory or imported at the "
                    f"top of the file."
                )
            continue

        # Direct Feature subclass reference
        if _is_feature_cls(hint):
            deps.add(hint)

    return deps


def topological_sort(
    classes: List[Type],
    dep_graph: Dict[Type, Set[Type]],
) -> Tuple[List[Type], Set[Tuple[Type, Type]]]:
    """Kahn's algorithm topological sort with cycle detection.

    Returns:
        sorted_classes: dependencies before dependents
        lazy_ref_pairs: set of (referencer, target) edges that form cycles
    """
    cls_set = set(classes)
    # Build in-degree and adjacency (only among known classes)
    in_degree: Dict[Type, int] = {c: 0 for c in classes}
    # edges: dep -> [cls that depends on dep]
    dependents: Dict[Type, List[Type]] = {c: [] for c in classes}

    for cls in classes:
        seen_deps: Set[Type] = set()
        for dep in dep_graph.get(cls, set()):
            if dep in cls_set and dep not in seen_deps:
                in_degree[cls] += 1
                dependents[dep].append(cls)
                seen_deps.add(dep)

    queue: deque[Type] = deque(c for c in classes if in_degree[c] == 0)
    sorted_list: List[Type] = []

    while queue:
        node = queue.popleft()
        sorted_list.append(node)
        for dependent in dependents[node]:
            in_degree[dependent] -= 1
            if in_degree[dependent] == 0:
                queue.append(dependent)

    # Classes not yet sorted are part of cycles
    remaining = [c for c in classes if c not in set(sorted_list)]

    # Identify cycle edges: any edge (referencer -> dep) where both are in remaining
    remaining_set = set(remaining)
    lazy_ref_pairs: Set[Tuple[Type, Type]] = set()
    for cls in remaining:
        for dep in dep_graph.get(cls, set()):
            if dep in remaining_set:
                lazy_ref_pairs.add((cls, dep))

    sorted_list.extend(remaining)
    return sorted_list, lazy_ref_pairs


# ─── Phase 3: Generation ──────────────────────────────────────────────────────


def classify_hint(
    hint: Any,
    ctx: CodegenContext,
    current_cls: Type,
    lazy_ref_pairs: Set[Tuple[Type, Type]],
) -> Tuple[str, str]:
    """Return (schema_entry_str, ts_type_str) for a single type hint."""

    resolve_ctx = ctx.resolve_ctx_for(current_cls)

    # Resolve string forward refs first
    hint = _resolve_hint(hint, resolve_ctx)

    # TypeVar hits
    if hint in _TYPEVAR_TO_TS:
        ts_name = _TYPEVAR_TO_TS[hint]
        ts_decl = _TYPEVAR_TS_DECL[hint]
        # REF bare → 'unknown | null'
        if hint is _fdb.REF:
            return ('REF', 'unknown | null')
        return (ts_name, ts_decl)

    # Native Python types
    if hint in _NATIVE_TO_TS_SCHEMA:
        return (_NATIVE_TO_TS_SCHEMA[hint], _NATIVE_TO_TS_DECL[hint])

    origin = get_origin(hint)

    # list[X] / List[X]
    if origin is list:
        args = get_args(hint)
        inner = _resolve_hint(args[0], resolve_ctx) if args else None
        if inner is None:
            return ('listOf(/* unknown */)', 'unknown[]')

        # Inner is a Feature class
        if _is_feature_cls(inner):
            name = inner.__name__
            if (current_cls, inner) in lazy_ref_pairs:
                schema = f'listOf(ref(() => {name}))'
            else:
                schema = f'listOf(ref({name}))'
            return (schema, f'{name}[]')

        # Inner is a scalar / TypeVar / native
        inner_schema, inner_decl = classify_hint(inner, ctx, current_cls, lazy_ref_pairs)
        # Strip ' | null' from inner_decl for array element type
        base_decl = inner_decl.replace(' | null', '')
        return (f'listOf({inner_schema})', f'{base_decl}[]')

    # Direct Feature subclass reference
    if _is_feature_cls(hint):
        name = hint.__name__
        if (current_cls, hint) in lazy_ref_pairs:
            return (f'ref(() => {name})', f'{name} | null')
        return (f'ref({name})', f'{name} | null')

    # Unresolvable string ref
    if isinstance(hint, str):
        print(f"Warning: unresolved forward reference '{hint}'", file=sys.stderr)
        return (f'/* unknown: {hint!r} */', 'unknown')

    # Fallback unknown
    print(f"Warning: unknown type hint {hint!r}", file=sys.stderr)
    return (f'/* unknown: {hint!r} */', 'unknown')


def _schema_token(entry: str) -> List[str]:
    """Extract top-level symbol names referenced in a schema entry string."""
    tokens = []
    for candidate in ['listOf', 'ref', 'BOOL', 'U8', 'U16', 'U32', 'I32',
                       'U8N', 'U16N', 'F32', 'F64', 'STR', 'WSTR', 'REF', 'BYTES']:
        if candidate in entry:
            tokens.append(candidate)
    return tokens


def collect_fastdb4ts_imports(
    classes: List[Type],
    ctx: CodegenContext,
    lazy_ref_pairs: Set[Tuple[Type, Type]],
) -> Set[str]:
    """Collect all symbols needed from fastdb4ts for the given classes."""
    symbols: Set[str] = {'Feature', 'defineSchema'}

    for cls in classes:
        hints = _get_hints_safe(cls, ctx)
        for field, hint in hints.items():
            if field.startswith('_'):
                continue
            try:
                schema_entry, _ = classify_hint(hint, ctx, cls, lazy_ref_pairs)
            except CodegenError:
                continue
            for token in _schema_token(schema_entry):
                symbols.add(token)

    return symbols


def generate_class(
    cls: Type,
    ctx: CodegenContext,
    lazy_ref_pairs: Set[Tuple[Type, Type]],
    current_py_file: Path,
    output_dir: Path,
) -> Tuple[str, Set[Tuple[Path, str]]]:
    """Generate TypeScript class code.

    Returns (ts_code, cross_file_imports).
    cross_file_imports: set of (source_py_file_relative, class_name) for classes
    defined in other files that this class references.
    """
    cross_file_imports: Set[Tuple[Path, str]] = set()

    hints = _get_hints_safe(cls, ctx)
    if not hints and cls.__annotations__:
        print(f"Warning: get_type_hints failed for {cls.__name__}", file=sys.stderr)

    resolve_ctx = ctx.resolve_ctx_for(cls)
    schema_lines: List[str] = []
    declare_lines: List[str] = []

    for field, hint in hints.items():
        if field.startswith('_'):
            continue
        try:
            schema_entry, ts_type = classify_hint(hint, ctx, cls, lazy_ref_pairs)
        except CodegenError as e:
            print(f"Warning: {e}", file=sys.stderr)
            schema_entry, ts_type = ('/* unknown */', 'unknown')

        schema_lines.append(f'    {field}: {schema_entry},')
        declare_lines.append(f'  declare {field}: {ts_type};')

        # Collect cross-file imports using canonicalization
        resolved_for_import = _resolve_hint(hint, resolve_ctx)
        origin_for_import = get_origin(resolved_for_import)
        inner_for_import = resolved_for_import
        if origin_for_import is list:
            args = get_args(resolved_for_import)
            inner_for_import = _resolve_hint(args[0], resolve_ctx) if args else None

        ref_cls: Optional[Type] = None
        if inner_for_import is not None and _is_feature_cls(inner_for_import):
            ref_cls = ctx.canonicalize(inner_for_import)
        elif _is_feature_cls(hint):
            ref_cls = ctx.canonicalize(hint)

        if ref_cls is not None:
            ref_file = ctx.get_file(ref_cls)
            if ref_file is not None and ref_file != current_py_file:
                cross_file_imports.add((ref_file, ref_cls.__name__))

    schema_body = '\n'.join(schema_lines) if schema_lines else ''
    declares = '\n'.join(declare_lines)

    lines = [f'export class {cls.__name__} extends Feature {{']
    lines.append(f'  static schema = defineSchema({{')
    if schema_body:
        lines.append(schema_body)
    lines.append(f'  }});')
    if declare_lines:
        lines.append(declares)
    lines.append('}')

    return '\n'.join(lines), cross_file_imports


def generate_file(
    py_file: Path,
    classes: List[Type],
    all_classes_sorted: List[Type],
    ctx: CodegenContext,
    lazy_ref_pairs: Set[Tuple[Type, Type]],
    output_dir: Path,
) -> str:
    """Assemble the full .ts file content for a given .py file."""
    # Determine output path for this file
    rel = py_file.relative_to(ctx.input_dir)
    out_ts = output_dir / rel.with_suffix('.ts')

    # Collect all needed fastdb4ts symbols
    symbols = collect_fastdb4ts_imports(classes, ctx, lazy_ref_pairs)
    sorted_symbols = sorted(symbols)

    # Gather cross-file imports
    all_cross: Set[Tuple[Path, str]] = set()
    class_blocks: List[str] = []

    # Only generate classes that belong to this file, in topological order
    file_classes_ordered = [c for c in all_classes_sorted if c in set(classes)]

    for cls in file_classes_ordered:
        code, cross = generate_class(
            cls, ctx, lazy_ref_pairs,
            rel, output_dir,
        )
        class_blocks.append(code)
        all_cross.update(cross)

    # Build cross-file import statements
    # Group by source file
    imports_by_file: Dict[Path, List[str]] = {}
    for src_rel, cls_name in sorted(all_cross):
        imports_by_file.setdefault(src_rel, []).append(cls_name)

    cross_import_lines: List[str] = []
    for src_rel in sorted(imports_by_file.keys()):
        names = sorted(imports_by_file[src_rel])
        # Compute relative path from output TS file dir to target TS file
        target_ts = output_dir / src_rel.with_suffix('.js')
        rel_path = os.path.relpath(target_ts, out_ts.parent)
        # Ensure forward slashes and leading ./
        rel_path = rel_path.replace(os.sep, '/')
        if not rel_path.startswith('.'):
            rel_path = './' + rel_path
        cross_import_lines.append(f"import {{ {', '.join(names)} }} from '{rel_path}';")

    parts: List[str] = []
    parts.append('// Auto-generated by fdb codegen --ts. Do not edit manually.')
    parts.append(f"import {{ {', '.join(sorted_symbols)} }} from 'fastdb4ts';")
    for line in cross_import_lines:
        parts.append(line)
    parts.append('')
    parts.append('\n\n'.join(class_blocks))

    return '\n'.join(parts) + '\n'


# ─── Main entrypoint ──────────────────────────────────────────────────────────


def run_codegen_ts(input_dir_str: str, output_dir_str: str) -> None:
    """Main entrypoint: transpile all Feature subclasses found under input_dir to TypeScript."""
    input_dir = Path(input_dir_str).resolve()
    if not input_dir.exists() or not input_dir.is_dir():
        print(f"Error: input directory '{input_dir}' does not exist or is not a directory.", file=sys.stderr)
        sys.exit(1)

    output_dir = Path(output_dir_str).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # Phase 1: Discovery
    file_to_classes, class_to_file, errors = discover_all(input_dir)

    for err in errors:
        print(f"Warning: {err}", file=sys.stderr)

    if not class_to_file:
        print("No Feature subclasses found.", file=sys.stderr)

    # Build context for resolution and canonicalization
    ctx = CodegenContext(file_to_classes, class_to_file, input_dir)

    # Phase 2: Analysis
    dep_graph: Dict[Type, Set[Type]] = {}
    all_classes = ctx.all_classes

    for cls in all_classes:
        try:
            dep_graph[cls] = build_dep_graph(cls, ctx)
        except CodegenError as e:
            print(f"Warning: {e}", file=sys.stderr)
            dep_graph[cls] = set()

    all_sorted, lazy_ref_pairs = topological_sort(all_classes, dep_graph)

    # Phase 3: Generation — use file_to_classes from discovery,
    # but reorder classes within each file by topological sort
    sorted_file_to_classes: Dict[Path, List[Type]] = {}
    for cls in all_sorted:
        src_rel = ctx.get_file(cls)
        if src_rel is None:
            continue
        sorted_file_to_classes.setdefault(src_rel, []).append(cls)

    # Also include py files that were loaded successfully but have no classes
    for py_file in scan_py_files(input_dir):
        rel = py_file.relative_to(input_dir)
        if rel not in sorted_file_to_classes:
            # Emit minimal file just to acknowledge processing
            out_ts = output_dir / rel.with_suffix('.ts')
            out_ts.parent.mkdir(parents=True, exist_ok=True)
            out_ts.write_text(
                '// Auto-generated by fdb codegen --ts. Do not edit manually.\n'
                "import { Feature, defineSchema } from 'fastdb4ts';\n"
            )

    generated = 0
    for src_rel, classes in sorted_file_to_classes.items():
        py_file = input_dir / src_rel
        content = generate_file(
            py_file, classes, all_sorted,
            ctx, lazy_ref_pairs, output_dir,
        )
        out_ts = output_dir / src_rel.with_suffix('.ts')
        out_ts.parent.mkdir(parents=True, exist_ok=True)
        out_ts.write_text(content)
        generated += 1

    print(f"Generated {generated} file(s) in {output_dir}")
