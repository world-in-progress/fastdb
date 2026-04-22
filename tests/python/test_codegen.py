"""Comprehensive tests for fdb codegen --ts (Python → TypeScript Feature transpiler)."""

import io
import os
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from typing import List
from unittest.mock import patch

from fastdb4py.codegen.ts_gen import (
    CodegenContext,
    CodegenError,
    build_dep_graph,
    classify_hint,
    collect_fastdb4ts_imports,
    discover_all,
    discover_features,
    generate_class,
    generate_file,
    load_module,
    run_codegen_ts,
    scan_py_files,
    topological_sort,
)
import fastdb4py as fdb
from fastdb4py.decorator import feature as _feature_decorator


def _make_feature_cls(name: str) -> type:
    """Create a synthetic @feature-decorated class for codegen unit tests."""
    cls = type(name, (), {"__annotations__": {}})
    return _feature_decorator(cls)


# ─── Helpers ──────────────────────────────────────────────────────────────────


def _write_files(tmpdir: Path, files: dict[str, str]) -> None:
    """Write {relative_path: content} into tmpdir, creating subdirs as needed."""
    for rel, content in files.items():
        p = tmpdir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(textwrap.dedent(content))


def _discover(files: dict[str, str]):
    """Write files to a temp dir, run discover_all, return (tmpdir, ctx, errors).

    ctx is a CodegenContext. Caller must keep the returned tmpdir reference alive.
    """
    td = tempfile.TemporaryDirectory()
    input_dir = Path(td.name)
    _write_files(input_dir, files)
    file_to_classes, class_to_file, errors = discover_all(input_dir)
    ctx = CodegenContext(file_to_classes, class_to_file, input_dir)
    return td, ctx, errors


# ─── Phase 1: Discovery ──────────────────────────────────────────────────────


class TestScanPyFiles(unittest.TestCase):
    def test_finds_py_files(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "a.py").touch()
            (Path(d) / "b.py").touch()
            result = scan_py_files(Path(d))
            names = [p.name for p in result]
            self.assertIn("a.py", names)
            self.assertIn("b.py", names)

    def test_excludes_dunder_files(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "__init__.py").touch()
            (Path(d) / "__main__.py").touch()
            (Path(d) / "features.py").touch()
            result = scan_py_files(Path(d))
            names = [p.name for p in result]
            self.assertEqual(names, ["features.py"])

    def test_recursive(self):
        with tempfile.TemporaryDirectory() as d:
            sub = Path(d) / "sub"
            sub.mkdir()
            (Path(d) / "top.py").touch()
            (sub / "nested.py").touch()
            result = scan_py_files(Path(d))
            names = sorted(p.name for p in result)
            self.assertEqual(names, ["nested.py", "top.py"])

    def test_empty_dir(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(scan_py_files(Path(d)), [])

    def test_sorted_order(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "c.py").touch()
            (Path(d) / "a.py").touch()
            (Path(d) / "b.py").touch()
            result = scan_py_files(Path(d))
            self.assertEqual([p.name for p in result], ["a.py", "b.py", "c.py"])


class TestLoadModule(unittest.TestCase):
    def test_loads_valid_file(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "ok.py"
            f.write_text("X = 42\n")
            mod, err = load_module(f, Path(d))
            self.assertIsNone(err)
            self.assertIsNotNone(mod)
            self.assertEqual(mod.X, 42)

    def test_syntax_error(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "bad.py"
            f.write_text("def oops(\n")
            mod, err = load_module(f, Path(d))
            self.assertIsNone(mod)
            self.assertIn("SyntaxError", err)

    def test_import_error(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "bad_import.py"
            f.write_text("import nonexistent_pkg_xyz\n")
            mod, err = load_module(f, Path(d))
            self.assertIsNone(mod)
            self.assertTrue("Import" in err or "Error" in err)

    def test_module_name_from_path(self):
        with tempfile.TemporaryDirectory() as d:
            sub = Path(d) / "sub"
            sub.mkdir()
            f = sub / "model.py"
            f.write_text("VAL = 1\n")
            mod, err = load_module(f, Path(d))
            self.assertIsNone(err)
            self.assertEqual(mod.__name__, "_fdb_codegen.sub.model")


class TestDiscoverFeatures(unittest.TestCase):
    def test_finds_feature_subclasses(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64
            @feature
            class Point:
                x: F64
        """})
        names = [c.__name__ for c in ctx.all_classes]
        self.assertIn("Point", names)
        td.cleanup()

    def test_ignores_imported_features(self):
        """Features imported from another file should not be re-discovered."""
        td, ctx, _ = _discover({
            "base.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64
                @feature
                class Point:
                    x: F64
            """,
            "consumer.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import STR
                import sys, os
                sys.path.insert(0, os.path.dirname(__file__))
                from base import Point
                @feature
                class Scene:
                    name: STR
            """,
        })
        # Point should only map to base.py
        point_classes = [c for c in ctx.all_classes if c.__name__ == "Point"]
        self.assertEqual(len(point_classes), 1)
        self.assertEqual(ctx.get_file(point_classes[0]), Path("base.py"))
        td.cleanup()

    def test_ignores_non_feature_classes(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64
            class Helper:
                pass
            @feature
            class Point:
                x: F64
        """})
        names = [c.__name__ for c in ctx.all_classes]
        self.assertNotIn("Helper", names)
        self.assertIn("Point", names)
        td.cleanup()

    def test_empty_module(self):
        td, ctx, _ = _discover({"f.py": "# no classes here\n"})
        self.assertEqual(len(ctx.all_classes), 0)
        td.cleanup()

    def test_multiple_classes(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64, I32
            @feature
            class A:
                x: F64
            @feature
            class B:
                y: I32
        """})
        names = [c.__name__ for c in ctx.all_classes]
        self.assertIn("A", names)
        self.assertIn("B", names)
        td.cleanup()


class TestDiscoverAll(unittest.TestCase):
    def test_basic_discovery(self):
        td, ctx, errors = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64
            @feature
            class Point:
                x: F64
                y: F64
        """})
        self.assertEqual(errors, [])
        names = [c.__name__ for c in ctx.all_classes]
        self.assertIn("Point", names)
        td.cleanup()

    def test_duplicate_names_across_files_all_generated(self):
        """Same class name in different files should both be discovered."""
        with tempfile.TemporaryDirectory() as d:
            _write_files(Path(d), {
                "a.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class Point:
                        x: F64
                """,
                "b.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class Point:
                        y: F64
                """,
            })
            file_to_classes, class_to_file, errors = discover_all(Path(d))
            # Both Points should be discovered
            all_names = [c.__name__ for c in class_to_file]
            self.assertEqual(all_names.count("Point"), 2)
            # Each in its own file
            files_with_point = set()
            for cls, rel in class_to_file.items():
                if cls.__name__ == "Point":
                    files_with_point.add(str(rel))
            self.assertEqual(files_with_point, {"a.py", "b.py"})

    def test_errors_returned_for_bad_files(self):
        with tempfile.TemporaryDirectory() as d:
            _write_files(Path(d), {
                "good.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class A:
                        x: F64
                """,
                "bad.py": "def oops(\n",
            })
            file_to_classes, class_to_file, errors = discover_all(Path(d))
            self.assertTrue(any("SyntaxError" in e for e in errors))
            names = [c.__name__ for c in class_to_file]
            self.assertIn("A", names)

    def test_class_to_file_mapping(self):
        td, ctx, _ = _discover({"models.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64
            @feature
            class Pt:
                x: F64
        """})
        pt_cls = [c for c in ctx.all_classes if c.__name__ == "Pt"][0]
        self.assertEqual(ctx.get_file(pt_cls), Path("models.py"))
        td.cleanup()

    def test_empty_dir(self):
        with tempfile.TemporaryDirectory() as d:
            file_to_classes, class_to_file, errors = discover_all(Path(d))
            self.assertEqual(len(class_to_file), 0)
            self.assertEqual(len(errors), 0)


# ─── Phase 2: Analysis ───────────────────────────────────────────────────────


class TestBuildDepGraph(unittest.TestCase):
    def test_no_deps(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64, STR
            @feature
            class Point:
                x: F64
                y: F64
                name: STR
        """})
        pt = [c for c in ctx.all_classes if c.__name__ == "Point"][0]
        deps = build_dep_graph(pt, ctx)
        self.assertEqual(deps, set())
        td.cleanup()

    def test_direct_feature_ref(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64, I32
            @feature
            class Point:
                x: F64
            @feature
            class Line:
                id: I32
                origin: Point
        """})
        line = [c for c in ctx.all_classes if c.__name__ == "Line"][0]
        deps = build_dep_graph(line, ctx)
        self.assertEqual({c.__name__ for c in deps}, {"Point"})
        td.cleanup()

    def test_list_of_feature(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64, I32
            from typing import List
            @feature
            class Point:
                x: F64
            @feature
            class Line:
                id: I32
                points: List[Point]
        """})
        line = [c for c in ctx.all_classes if c.__name__ == "Line"][0]
        deps = build_dep_graph(line, ctx)
        self.assertEqual({c.__name__ for c in deps}, {"Point"})
        td.cleanup()

    def test_list_of_scalar_no_deps(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64
            from typing import List
            @feature
            class Data:
                values: List[F64]
        """})
        data = [c for c in ctx.all_classes if c.__name__ == "Data"][0]
        deps = build_dep_graph(data, ctx)
        self.assertEqual(deps, set())
        td.cleanup()

    def test_self_ref(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import I32
            @feature
            class Node:
                val: I32
                next: 'Node'
        """})
        node = [c for c in ctx.all_classes if c.__name__ == "Node"][0]
        deps = build_dep_graph(node, ctx)
        self.assertEqual({c.__name__ for c in deps}, {"Node"})
        td.cleanup()


class TestTopologicalSort(unittest.TestCase):
    def test_no_deps_all_present(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64, I32
            @feature
            class A:
                x: F64
            @feature
            class B:
                y: I32
        """})
        dep_graph = {c: build_dep_graph(c, ctx) for c in ctx.all_classes}
        sorted_cls, lazy = topological_sort(ctx.all_classes, dep_graph)
        self.assertEqual({c.__name__ for c in sorted_cls}, {"A", "B"})
        self.assertEqual(lazy, set())
        td.cleanup()

    def test_dep_before_dependent(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64, I32
            from typing import List
            @feature
            class Point:
                x: F64
            @feature
            class Line:
                id: I32
                points: List[Point]
        """})
        dep_graph = {c: build_dep_graph(c, ctx) for c in ctx.all_classes}
        sorted_cls, _ = topological_sort(ctx.all_classes, dep_graph)
        names = [c.__name__ for c in sorted_cls]
        self.assertLess(names.index("Point"), names.index("Line"))
        td.cleanup()

    def test_self_ref_detected(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import I32
            @feature
            class Node:
                val: I32
                next: 'Node'
        """})
        dep_graph = {c: build_dep_graph(c, ctx) for c in ctx.all_classes}
        sorted_cls, lazy = topological_sort(ctx.all_classes, dep_graph)
        lazy_names = {(a.__name__, b.__name__) for a, b in lazy}
        self.assertIn(("Node", "Node"), lazy_names)
        td.cleanup()

    def test_mutual_cycle(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import I32
            @feature
            class A:
                val: I32
                other: 'B'
            @feature
            class B:
                val: I32
                other: 'A'
        """})
        dep_graph = {c: build_dep_graph(c, ctx) for c in ctx.all_classes}
        sorted_cls, lazy = topological_sort(ctx.all_classes, dep_graph)
        # Both classes present
        self.assertEqual({c.__name__ for c in sorted_cls}, {"A", "B"})
        # At least one lazy pair exists
        self.assertTrue(len(lazy) > 0)
        td.cleanup()

    def test_diamond_dependency(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import I32
            @feature
            class D:
                val: I32
            @feature
            class B:
                d: D
            @feature
            class C:
                d: D
            @feature
            class A:
                b: B
                c: C
        """})
        dep_graph = {c: build_dep_graph(c, ctx) for c in ctx.all_classes}
        sorted_cls, lazy = topological_sort(ctx.all_classes, dep_graph)
        names = [c.__name__ for c in sorted_cls]
        self.assertLess(names.index("D"), names.index("B"))
        self.assertLess(names.index("D"), names.index("C"))
        self.assertLess(names.index("B"), names.index("A"))
        self.assertLess(names.index("C"), names.index("A"))
        self.assertEqual(lazy, set())
        td.cleanup()


# ─── Phase 3: Generation ─────────────────────────────────────────────────────


class TestClassifyHint(unittest.TestCase):
    """classify_hint returns (schema_entry, ts_type) for a type annotation."""

    @staticmethod
    def _make_ctx(registry: dict = None) -> CodegenContext:
        """Build a minimal CodegenContext from an optional name→class dict."""
        reg = registry or {}
        dummy_path = Path("_test_.py")
        file_to_classes = {dummy_path: list(reg.values())} if reg else {}
        class_to_file = {cls: dummy_path for cls in reg.values()}
        return CodegenContext(file_to_classes, class_to_file, Path("."))

    def _ch(self, hint, registry=None, current_cls=None, lazy=None):
        ctx = self._make_ctx(registry)
        return classify_hint(hint, ctx, current_cls, lazy or set())

    # TypeVar scalars
    def test_f64(self):
        self.assertEqual(self._ch(fdb.F64), ("F64", "number"))

    def test_u32(self):
        self.assertEqual(self._ch(fdb.U32), ("U32", "number"))

    def test_i32(self):
        self.assertEqual(self._ch(fdb.I32), ("I32", "number"))

    def test_str_typevar(self):
        self.assertEqual(self._ch(fdb.STR), ("STR", "string"))

    def test_wstr_typevar(self):
        self.assertEqual(self._ch(fdb.WSTR), ("WSTR", "string"))

    def test_bool_typevar(self):
        self.assertEqual(self._ch(fdb.BOOL), ("BOOL", "boolean"))

    def test_u8n_typevar(self):
        self.assertEqual(self._ch(fdb.U8N), ("U8N", "number"))

    def test_u16n_typevar(self):
        self.assertEqual(self._ch(fdb.U16N), ("U16N", "number"))

    def test_bytes_typevar(self):
        self.assertEqual(self._ch(fdb.BYTES), ("BYTES", "Uint8Array"))

    def test_ref_bare_typevar(self):
        self.assertEqual(self._ch(fdb.REF), ("REF", "unknown | null"))

    # Native Python types
    def test_native_int(self):
        self.assertEqual(self._ch(int), ("I32", "number"))

    def test_native_float(self):
        self.assertEqual(self._ch(float), ("F64", "number"))

    def test_native_str(self):
        self.assertEqual(self._ch(str), ("STR", "string"))

    def test_native_bool(self):
        self.assertEqual(self._ch(bool), ("BOOL", "boolean"))

    # Feature ref
    def test_feature_ref_direct(self):
        Pt = _make_feature_cls("Point")
        reg = {"Point": Pt}
        self.assertEqual(self._ch(Pt, reg), ("ref(Point)", "Point | null"))

    def test_feature_ref_lazy(self):
        Pt = _make_feature_cls("Point")
        reg = {"Point": Pt}
        lazy = {(None, Pt)}
        self.assertEqual(
            self._ch(Pt, reg, None, lazy),
            ("ref(() => Point)", "Point | null"),
        )

    # List types
    def test_list_f64(self):
        self.assertEqual(self._ch(List[fdb.F64]), ("listOf(F64)", "number[]"))

    def test_list_str_native(self):
        self.assertEqual(self._ch(List[str]), ("listOf(STR)", "string[]"))

    def test_list_int_native(self):
        self.assertEqual(self._ch(List[int]), ("listOf(I32)", "number[]"))

    def test_list_feature(self):
        Pt = _make_feature_cls("Point")
        reg = {"Point": Pt}
        self.assertEqual(
            self._ch(List[Pt], reg),
            ("listOf(ref(Point))", "Point[]"),
        )

    def test_list_feature_lazy(self):
        Pt = _make_feature_cls("Point")
        reg = {"Point": Pt}
        lazy = {(None, Pt)}
        self.assertEqual(
            self._ch(List[Pt], reg, None, lazy),
            ("listOf(ref(() => Point))", "Point[]"),
        )

    # Unknown type
    def test_unknown_warns(self):
        with patch("sys.stderr", new_callable=io.StringIO) as err:
            schema, ts = self._ch(dict)
        self.assertIn("unknown", schema.lower())
        self.assertEqual(ts, "unknown")


class TestCollectFastdb4tsImports(unittest.TestCase):
    def test_always_includes_base_symbols(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64
            @feature
            class Pt:
                x: F64
        """})
        pt = [c for c in ctx.all_classes if c.__name__ == "Pt"][0]
        syms = collect_fastdb4ts_imports([pt], ctx, set())
        self.assertIn("Feature", syms)
        self.assertIn("defineSchema", syms)
        self.assertIn("F64", syms)
        td.cleanup()

    def test_includes_ref_and_listof(self):
        td, ctx, _ = _discover({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64, I32
            from typing import List
            @feature
            class Pt:
                x: F64
            @feature
            class Line:
                id: I32
                points: List[Pt]
        """})
        line = [c for c in ctx.all_classes if c.__name__ == "Line"][0]
        syms = collect_fastdb4ts_imports([line], ctx, set())
        self.assertIn("ref", syms)
        self.assertIn("listOf", syms)
        td.cleanup()


class TestGenerateClass(unittest.TestCase):
    def _gen(self, files, cls_name):
        td, ctx, _ = _discover(files)
        dep_graph = {c: build_dep_graph(c, ctx) for c in ctx.all_classes}
        sorted_cls, lazy = topological_sort(ctx.all_classes, dep_graph)
        cls = [c for c in ctx.all_classes if c.__name__ == cls_name][0]
        src_rel = ctx.get_file(cls)
        outdir = Path(td.name) / "_out"
        outdir.mkdir(exist_ok=True)
        code, cross = generate_class(cls, ctx, lazy, src_rel, outdir)
        td.cleanup()
        return code, cross

    def test_simple_scalars(self):
        code, _ = self._gen({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64
            @feature
            class Point:
                x: F64
                y: F64
        """}, "Point")
        self.assertIn("export class Point extends Feature", code)
        self.assertIn("x: F64,", code)
        self.assertIn("y: F64,", code)
        self.assertIn("declare x: number;", code)
        self.assertIn("declare y: number;", code)

    def test_native_types(self):
        code, _ = self._gen({"f.py": """\
            from fastdb4py.decorator import feature
            @feature
            class Data:
                count: int
                ratio: float
                label: str
        """}, "Data")
        self.assertIn("count: I32,", code)
        self.assertIn("ratio: F64,", code)
        self.assertIn("label: STR,", code)

    def test_ref_field(self):
        code, _ = self._gen({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64
            from typing import List
            @feature
            class Pt:
                x: F64
            @feature
            class Line:
                points: List[Pt]
        """}, "Line")
        self.assertIn("listOf(ref(Pt))", code)
        self.assertIn("declare points: Pt[];", code)

    def test_self_ref_lazy(self):
        code, _ = self._gen({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import I32
            @feature
            class Node:
                val: I32
                next: 'Node'
        """}, "Node")
        self.assertIn("ref(() => Node)", code)
        self.assertIn("declare next: Node | null;", code)

    def test_no_fields(self):
        code, _ = self._gen({"f.py": """\
            from fastdb4py.decorator import feature
            @feature
            class Empty:
                pass
        """}, "Empty")
        self.assertIn("defineSchema({", code)
        self.assertNotIn("declare", code)

    def test_output_has_export(self):
        code, _ = self._gen({"f.py": """\
            from fastdb4py.decorator import feature
            from fastdb4py.type import F64
            @feature
            class Pt:
                x: F64
        """}, "Pt")
        self.assertTrue(code.startswith("export class"))

    def test_cross_file_import_detected(self):
        """End-to-end cross-file test via run_codegen_ts."""
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            # Use a sub-package so intra-package import works reliably
            pkg = Path(indir) / "mypkg"
            pkg.mkdir()
            (pkg / "__init__.py").write_text("")
            _write_files(Path(indir), {
                "geometry.py": textwrap.dedent("""\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class Point:
                        x: F64
                        y: F64
                """),
                "scene.py": textwrap.dedent("""\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import STR
                    import sys, os
                    sys.path.insert(0, os.path.dirname(__file__))
                    from geometry import Point
                    @feature
                    class Scene:
                        name: STR
                        root: Point
                """),
            })
            # Clean cached modules that could interfere
            for key in list(sys.modules.keys()):
                if key.startswith("_fdb_codegen."):
                    del sys.modules[key]
            run_codegen_ts(indir, outdir)
            scene_ts = Path(outdir) / "scene.ts"
            self.assertTrue(scene_ts.exists())
            content = scene_ts.read_text()
            self.assertIn("from './geometry.js'", content)
            self.assertIn("Point", content)


# ─── End-to-end: run_codegen_ts ───────────────────────────────────────────────


class TestRunCodegenTs(unittest.TestCase):
    def test_creates_output_dir(self):
        with tempfile.TemporaryDirectory() as indir:
            outdir = Path(indir) / "nonexistent_out"
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64
                @feature
                class A:
                    x: F64
            """})
            run_codegen_ts(indir, str(outdir))
            self.assertTrue(outdir.is_dir())

    def test_generates_ts_file(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"geometry.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64
                @feature
                class Point:
                    x: F64
                    y: F64
            """})
            run_codegen_ts(indir, outdir)
            self.assertTrue((Path(outdir) / "geometry.ts").exists())

    def test_ts_file_content(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"geometry.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64
                @feature
                class Point:
                    x: F64
                    y: F64
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "geometry.ts").read_text()
            self.assertIn("F64", content)
            self.assertIn("defineSchema", content)
            self.assertIn("declare x: number;", content)
            self.assertIn("declare y: number;", content)

    def test_header_comment(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64
                @feature
                class A:
                    x: F64
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "f.ts").read_text()
            self.assertTrue(content.startswith("// Auto-generated by fdb codegen --ts."))

    def test_empty_dir_no_crash(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            with patch("sys.stderr", new_callable=io.StringIO) as err:
                run_codegen_ts(indir, outdir)
            self.assertIn("No Feature subclasses found", err.getvalue())

    def test_nonexistent_input_dir(self):
        with self.assertRaises(SystemExit) as ctx:
            run_codegen_ts("/nonexistent/path/xyz", "/tmp/out")
        self.assertEqual(ctx.exception.code, 1)

    def test_syntax_error_file_skipped(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {
                "good.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class A:
                        x: F64
                """,
                "bad.py": "def oops(\n",
            })
            with patch("sys.stderr", new_callable=io.StringIO) as err:
                run_codegen_ts(indir, outdir)
            self.assertIn("SyntaxError", err.getvalue())
            self.assertTrue((Path(outdir) / "good.ts").exists())

    def test_import_error_file_skipped(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {
                "good.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class A:
                        x: F64
                """,
                "bad_import.py": "import nonexistent_pkg_xyz_abc\n",
            })
            with patch("sys.stderr", new_callable=io.StringIO) as err:
                run_codegen_ts(indir, outdir)
            stderr_text = err.getvalue()
            self.assertTrue("Import" in stderr_text or "Error" in stderr_text)
            self.assertTrue((Path(outdir) / "good.ts").exists())

    def test_non_feature_classes_ignored(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64
                class Helper:
                    x = 1
                @feature
                class Pt:
                    x: F64
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "f.ts").read_text()
            self.assertNotIn("Helper", content)
            self.assertIn("Pt", content)

    def test_multiple_files_mirrored(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {
                "geometry.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class Pt:
                        x: F64
                """,
                "scene.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import STR
                    @feature
                    class Scene:
                        name: STR
                """,
            })
            run_codegen_ts(indir, outdir)
            self.assertTrue((Path(outdir) / "geometry.ts").exists())
            self.assertTrue((Path(outdir) / "scene.ts").exists())

    def test_cross_file_import_generated(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {
                "geometry.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class Point:
                        x: F64
                        y: F64
                """,
                "scene.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import STR
                    import sys, os
                    sys.path.insert(0, os.path.dirname(__file__))
                    from geometry import Point
                    @feature
                    class Scene:
                        name: STR
                        root: Point
                """,
            })
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "scene.ts").read_text()
            self.assertIn("from './geometry.js'", content)
            self.assertIn("import { Point }", content)

    def test_summary_printed(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64
                @feature
                class A:
                    x: F64
            """})
            with patch("sys.stdout", new_callable=io.StringIO) as out:
                run_codegen_ts(indir, outdir)
            self.assertIn("Generated", out.getvalue())
            self.assertIn("file(s)", out.getvalue())

    def test_topo_order_in_generated_file(self):
        """Dependency class appears before dependent class in the output."""
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64, I32
                from typing import List
                @feature
                class Line:
                    id: I32
                    points: List['Point']
                @feature
                class Point:
                    x: F64
                    y: F64
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "f.ts").read_text()
            # Point should appear before Line in generated output
            self.assertLess(content.index("class Point"), content.index("class Line"))


# ─── Robustness / edge cases ─────────────────────────────────────────────────


class TestRobustnessEdgeCases(unittest.TestCase):
    def test_deeply_nested_dir_structure(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"a/b/c/features.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64
                @feature
                class DeepPoint:
                    x: F64
            """})
            run_codegen_ts(indir, outdir)
            ts_file = Path(outdir) / "a" / "b" / "c" / "features.ts"
            self.assertTrue(ts_file.exists())
            self.assertIn("DeepPoint", ts_file.read_text())

    def test_class_with_all_scalar_types(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import U8, U16, U32, I32, U8N, U16N, F32, F64, STR, WSTR, BYTES, BOOL
                @feature
                class AllTypes:
                    a: U8
                    b: U16
                    c: U32
                    d: I32
                    e: U8N
                    f: U16N
                    g: F32
                    h: F64
                    i: STR
                    j: WSTR
                    k: BYTES
                    l: BOOL
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "f.ts").read_text()
            for sym in ["U8", "U16", "U32", "I32", "U8N", "U16N", "F32", "F64", "STR", "WSTR", "BYTES", "BOOL"]:
                self.assertIn(sym, content, f"Missing {sym} in output")

    def test_list_of_int_and_str(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from typing import List
                @feature
                class Data:
                    ints: List[int]
                    names: List[str]
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "f.ts").read_text()
            self.assertIn("listOf(I32)", content)
            self.assertIn("listOf(STR)", content)
            self.assertIn("declare ints: number[];", content)
            self.assertIn("declare names: string[];", content)

    def test_circular_ref_generates_lazy(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import I32
                @feature
                class A:
                    val: I32
                    other: 'B'
                @feature
                class B:
                    val: I32
                    other: 'A'
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "f.ts").read_text()
            # At least one lazy ref should be present
            self.assertIn("() =>", content)

    def test_file_with_only_imports_no_features(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"empty.py": """\
                from fastdb4py.decorator import feature
                # No Feature subclasses defined
                x = 1
            """})
            with patch("sys.stderr", new_callable=io.StringIO):
                run_codegen_ts(indir, outdir)
            ts = Path(outdir) / "empty.ts"
            self.assertTrue(ts.exists())
            content = ts.read_text()
            self.assertIn("Auto-generated", content)
            self.assertIn("fastdb4ts", content)

    def test_forward_string_ref_resolved(self):
        """Forward reference as string annotation should resolve when class exists."""
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import I32
                from typing import List
                @feature
                class TreeNode:
                    val: I32
                    children: List['TreeNode']
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "f.ts").read_text()
            self.assertIn("TreeNode", content)
            # Self-referential list should use lazy ref
            self.assertIn("() => TreeNode", content)

    def test_undeclared_type_warns_but_no_crash(self):
        """Undeclared type in annotation produces warning, not a crash.
        
        When get_type_hints() fails to resolve a forward ref, the class gets
        an empty schema (no fields). The key point is: no crash, file generated.
        """
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64
                @feature
                class Bad:
                    x: F64
                    mystery: 'Nonexistent'
            """})
            with patch("sys.stderr", new_callable=io.StringIO) as err:
                run_codegen_ts(indir, outdir)
            ts = Path(outdir) / "f.ts"
            self.assertTrue(ts.exists())
            content = ts.read_text()
            # File should still contain the class definition
            self.assertIn("class Bad extends Feature", content)
            # A warning should have been emitted
            self.assertTrue(len(err.getvalue()) > 0)

    def test_duplicate_class_across_files_both_generated(self):
        """Same class name in different files: both should be generated, no warning."""
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {
                "a.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class Dup:
                        x: F64
                """,
                "b.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import I32
                    @feature
                    class Dup:
                        y: I32
                """,
            })
            with patch("sys.stderr", new_callable=io.StringIO) as err:
                run_codegen_ts(indir, outdir)
            # No "multiple files" warning
            self.assertNotIn("multiple files", err.getvalue())
            # Both files generated
            a_ts = Path(outdir) / "a.ts"
            b_ts = Path(outdir) / "b.ts"
            self.assertTrue(a_ts.exists(), "a.ts should be generated")
            self.assertTrue(b_ts.exists(), "b.ts should be generated")
            # Each file contains its own Dup definition
            a_content = a_ts.read_text()
            b_content = b_ts.read_text()
            self.assertIn("x: F64", a_content)
            self.assertIn("y: I32", b_content)

    def test_bool_field(self):
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import BOOL
                @feature
                class Flags:
                    active: BOOL
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "f.ts").read_text()
            self.assertIn("active: BOOL", content)
            self.assertIn("declare active: boolean;", content)


    def test_within_file_duplicate_last_wins(self):
        """Python semantics: last class definition wins for same name in one file."""
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {"f.py": """\
                from fastdb4py.decorator import feature
                from fastdb4py.type import F64, I32
                @feature
                class Point:
                    x: F64
                @feature
                class Point:
                    y: I32
            """})
            run_codegen_ts(indir, outdir)
            content = (Path(outdir) / "f.ts").read_text()
            # Only the last definition (y: I32) should appear
            self.assertIn("y: I32", content)
            # First definition should NOT appear
            self.assertNotIn("x: F64", content)

    def test_duplicate_names_across_files_all_generated(self):
        """Same class name in 3 files: all 3 should generate independently."""
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {
                "a.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class Point:
                        x: F64
                """,
                "b.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import I32
                    @feature
                    class Point:
                        y: I32
                """,
                "c.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import STR
                    @feature
                    class Point:
                        label: STR
                """,
            })
            with patch("sys.stderr", new_callable=io.StringIO) as err:
                run_codegen_ts(indir, outdir)
            # No warnings
            self.assertNotIn("multiple files", err.getvalue())
            # All 3 generated
            for name, field in [("a.ts", "x: F64"), ("b.ts", "y: I32"), ("c.ts", "label: STR")]:
                ts = Path(outdir) / name
                self.assertTrue(ts.exists(), f"{name} should exist")
                self.assertIn(field, ts.read_text(), f"{name} should contain {field}")

    def test_duplicate_names_cross_file_import(self):
        """File A defines Point, file B also defines Point, file C imports from A.

        C's generated .ts should import Point from './a.js', not from './b.js'.
        """
        with tempfile.TemporaryDirectory() as indir, tempfile.TemporaryDirectory() as outdir:
            _write_files(Path(indir), {
                "a.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import F64
                    @feature
                    class Point:
                        x: F64
                """,
                "b.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import I32
                    @feature
                    class Point:
                        y: I32
                """,
                "c.py": """\
                    from fastdb4py.decorator import feature
                    from fastdb4py.type import STR
                    import sys, os
                    sys.path.insert(0, os.path.dirname(__file__))
                    from a import Point
                    @feature
                    class Scene:
                        name: STR
                        origin: Point
                """,
            })
            # Clean cached modules
            for key in list(sys.modules.keys()):
                if key.startswith("_fdb_codegen."):
                    del sys.modules[key]
            run_codegen_ts(indir, outdir)
            c_content = (Path(outdir) / "c.ts").read_text()
            # Should import from a.js (where the Point used by C is defined)
            self.assertIn("from './a.js'", c_content)
            # Should NOT import from b.js
            self.assertNotIn("from './b.js'", c_content)


if __name__ == "__main__":
    unittest.main()
