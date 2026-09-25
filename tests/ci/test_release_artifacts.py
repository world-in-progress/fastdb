#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest.mock import patch

import release_artifacts as release


class ReleaseInventoryTests(unittest.TestCase):
    def test_repaired_wheel_preserves_license_texts(self):
        with tempfile.TemporaryDirectory() as directory:
            wheel = Path(directory) / "fastdb4py-0.2.0-cp312-cp312-manylinux_2_17_x86_64.whl"
            for notice in (None, b"replaced original text", (release.ROOT / "THIRD_PARTY_NOTICES.txt").read_bytes()):
                with zipfile.ZipFile(wheel, "w") as archive:
                    archive.writestr("fastdb4py-0.2.0.dist-info/METADATA", "Name: fastdb4py\nVersion: 0.2.0\n")
                    archive.writestr("fastdb4py/payload/__init__.py", "")
                    archive.writestr("fastdb4py-0.2.0.dist-info/licenses/LICENSE", (release.ROOT / "LICENSE").read_bytes())
                    if notice is not None:
                        archive.writestr("fastdb4py-0.2.0.dist-info/licenses/THIRD_PARTY_NOTICES.txt", notice)
                if notice is None or notice == b"replaced original text":
                    with self.assertRaisesRegex(release.ReleaseError, "original THIRD_PARTY_NOTICES"):
                        release.inspect_wheel(wheel, "0.2.0")
                else:
                    self.assertEqual(release.inspect_wheel(wheel, "0.2.0"), "cp312")

    def test_third_party_notice_matches_repository_originals(self):
        subprocess.run([
            sys.executable,
            str(release.ROOT / "tools/generate_third_party_notices.py"),
            "--check",
        ], check=True)

    def test_source_provenance_rejects_dirty_tracked_files_but_allows_wheelhouse(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            source = root / "source.cpp"
            source.write_text("tested source")
            subprocess.run(["git", "add", "source.cpp"], cwd=root, check=True)
            subprocess.run(["git", "-c", "user.name=Release Test", "-c", "user.email=release@example.invalid", "commit", "--quiet", "-m", "source"], cwd=root, check=True)
            (root / "wheelhouse").mkdir()
            (root / "wheelhouse/test.whl").write_bytes(b"build output")
            with patch.object(release, "ROOT", root):
                self.assertEqual(len(release.source_sha()), 40)
                source.write_text("uncommitted change")
                with self.assertRaisesRegex(release.ReleaseError, "committed and clean"):
                    release.source_sha()

    def test_core_bundle_retains_headers_licenses_and_hashed_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            build.mkdir()
            library = build / "libfastdb.so"
            library.write_bytes(b"tested native library")
            release.write_json(build / "release-build-source.json", {"source_sha": "a" * 40, "source_root": str(release.ROOT.resolve())})
            with patch.object(release, "source_sha", return_value="a" * 40), patch.object(release, "version", return_value="0.2.0"), patch.object(release.platform, "machine", return_value="x86_64"), patch.object(release.platform, "platform", return_value="Linux"), patch.object(release.platform, "libc_ver", return_value=("glibc", "2.39")), patch.object(release.subprocess, "check_output", return_value="compiler version"), patch("check_rust_payload_package.locate_system_library", return_value=library):
                release.make_core(build, root / "dist", release.TARGETS[0])
            with tarfile.open(root / "dist/fastdb-core-0.2.0-x86_64-unknown-linux-gnu.tar.gz") as archive:
                members = set(archive.getnames())
                for expected in ("LICENSE", "THIRD_PARTY_NOTICES.txt", "include/fastdb_payload.h", "include/fastdb_payload.hpp", "licenses/double-conversion/LICENSE", "licenses/yyjson/UPSTREAM.md", "licenses/picosha2/LICENSE", "licenses/clipper/NOTICES.txt", "licenses/gaiageo/NOTICES.txt"):
                    self.assertIn(expected, members)
                manifest = json.load(archive.extractfile("manifest.json"))
                inventory = {item["path"] for item in manifest["files"]}
                self.assertIn("licenses/double-conversion/LICENSE", inventory)
                self.assertIn("include/fastdb_payload.hpp", inventory)

    def candidate(self, root: Path) -> dict:
        records = []
        platform_tags = {
            "x86_64-unknown-linux-gnu": "manylinux_2_28_x86_64",
            "aarch64-apple-darwin": "macosx_11_0_arm64",
            "x86_64-pc-windows-msvc": "win_amd64",
        }
        for target in release.TARGETS:
            core = root / f"fastdb-core-0.2.0-{target}.tar.gz"
            core.write_bytes(b"tested core")
            records.append(release.file_record(core, kind="core-bundle", target=target))
            for abi in release.PYTHON_ABIS:
                wheel = root / f"fastdb4py-0.2.0-{abi}-{abi}-{platform_tags[target]}.whl"
                wheel.write_bytes(b"tested wheel")
                record = release.file_record(wheel, kind="python-wheel", target=target)
                record["python_abi"] = abi
                records.append(record)
        for name, kind in (("fastdb-0.2.0.crate", "rust-crate"), ("fastdb-sys-0.2.0.crate", "rust-crate"), ("fastdb4py-0.2.0.tar.gz", "python-sdist"), ("fastdb4ts-0.2.0.tgz", "typescript-package")):
            path = root / name
            path.write_bytes(b"tested package")
            records.append(release.file_record(path, kind=kind))
        document = {"schema": release.SCHEMA, "version": "0.2.0", "source_sha": "a" * 40, "artifacts": records}
        release.write_json(root / "release-manifest.json", document)
        return document

    def test_verification_rejects_changed_source_or_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = self.candidate(root)
            self.assertEqual(release.verify(root, "a" * 40), manifest)
            with self.assertRaisesRegex(release.ReleaseError, "source SHA"):
                release.verify(root, "b" * 40)
            (root / "fastdb-0.2.0.crate").write_bytes(b"repacked after testing")
            with self.assertRaisesRegex(release.ReleaseError, "artifact mismatch"):
                release.verify(root, "a" * 40)

    def test_incomplete_abi_matrix_cannot_be_published(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            document = self.candidate(root)
            document["artifacts"] = [record for record in document["artifacts"] if record.get("python_abi") != "cp310"]
            release.write_json(root / "release-manifest.json", document)
            with self.assertRaisesRegex(release.ReleaseError, "Incomplete Python ABI"):
                release.verify(root, "a" * 40)

    def test_duplicate_and_escaping_artifact_names_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            document = self.candidate(root)
            document["artifacts"].append(document["artifacts"][0])
            release.write_json(root / "release-manifest.json", document)
            with self.assertRaisesRegex(release.ReleaseError, "duplicate"):
                release.verify(root, "a" * 40)
            document["artifacts"].pop()
            document["artifacts"][0]["name"] = "../outside.tar.gz"
            release.write_json(root / "release-manifest.json", document)
            with self.assertRaisesRegex(release.ReleaseError, "filename"):
                release.verify(root, "a" * 40)


    def test_windows_core_bundle_pairs_import_library_with_runtime_dll(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            (build / "fastcarto" / "fastdb").mkdir(parents=True)
            library = build / "fastcarto" / "fastdb" / "fastdb.lib"
            library.write_bytes(b"import library")
            runtime = build / "fastcarto" / "fastdb" / "fastdb.dll"
            runtime.write_bytes(b"runtime library")
            release.write_json(build / "release-build-source.json", {"source_sha": "a" * 40, "source_root": str(release.ROOT.resolve())})
            cache = build / "CMakeFiles" / "3.30" / "CMakeCXXCompiler.cmake"
            cache.parent.mkdir(parents=True)
            cache.write_text(
                'set(CMAKE_CXX_COMPILER "C:/VS/VC/Tools/MSVC/bin/Hostx64/x64/cl.exe")\n'
                'set(CMAKE_CXX_COMPILER_ID "MSVC")\n'
                'set(CMAKE_CXX_COMPILER_VERSION "19.42.34435.0")\n'
            )
            with patch.object(release, "source_sha", return_value="a" * 40), patch.object(release, "version", return_value="0.2.0"), patch.object(release.platform, "machine", return_value="AMD64"), patch.object(release.platform, "platform", return_value="Windows-10"), patch.object(release.platform, "libc_ver", return_value=("", "")), patch("check_rust_payload_package.locate_system_library", return_value=library):
                release.make_core(build, root / "dist", release.WINDOWS_TARGET)
            with tarfile.open(root / "dist/fastdb-core-0.2.0-x86_64-pc-windows-msvc.tar.gz") as archive:
                members = set(archive.getnames())
                self.assertIn("lib/fastdb.lib", members)
                self.assertIn("lib/fastdb.dll", members)
                for expected in ("LICENSE", "THIRD_PARTY_NOTICES.txt", "licenses/clipper/NOTICES.txt", "licenses/gaiageo/NOTICES.txt"):
                    self.assertIn(expected, members)
                manifest = json.load(archive.extractfile("manifest.json"))
                self.assertEqual(manifest["windows_import_library"], "lib/fastdb.lib")
                self.assertEqual(manifest["windows_runtime_dll"], "lib/fastdb.dll")
                self.assertIn("MSVC 19.42.34435.0", manifest["compiler"])
                inventory = {item["path"] for item in manifest["files"]}
                self.assertIn("lib/fastdb.dll", inventory)

    def test_windows_core_bundle_requires_runtime_dll_and_msvc_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build" / "fastcarto" / "fastdb"
            build.mkdir(parents=True)
            library = build / "fastdb.lib"
            library.write_bytes(b"import library")
            release.write_json(build.parent.parent / "release-build-source.json", {"source_sha": "a" * 40, "source_root": str(release.ROOT.resolve())})
            patches = (
                patch.object(release, "source_sha", return_value="a" * 40),
                patch.object(release, "version", return_value="0.2.0"),
                patch.object(release.platform, "machine", return_value="AMD64"),
                patch.object(release.platform, "platform", return_value="Windows-10"),
                patch.object(release.platform, "libc_ver", return_value=("", "")),
                patch("check_rust_payload_package.locate_system_library", return_value=library),
            )
            with patches[0], patches[1], patches[2], patches[3], patches[4], patches[5]:
                with self.assertRaisesRegex(release.ReleaseError, "fastdb.dll"):
                    release.make_core(build.parent.parent, root / "dist", release.WINDOWS_TARGET)
                (build / "fastdb.dll").write_bytes(b"runtime library")
                with self.assertRaisesRegex(release.ReleaseError, "CMakeCXXCompiler.cmake"):
                    release.make_core(build.parent.parent, root / "dist2", release.WINDOWS_TARGET)
            with patch.object(release, "source_sha", return_value="a" * 40), patch.object(release, "version", return_value="0.2.0"), patch.object(release.platform, "machine", return_value="arm64"), patch.object(release.platform, "platform", return_value="Windows-10"), patch.object(release.platform, "libc_ver", return_value=("", "")):
                with self.assertRaisesRegex(release.ReleaseError, "native"):
                    release.make_core(build.parent.parent, root / "dist3", release.WINDOWS_TARGET)

    def test_windows_wheel_part_requires_win_amd64_tag(self):
        def write_wheel(directory: Path, name: str) -> None:
            with zipfile.ZipFile(directory / name, "w") as archive:
                archive.writestr("fastdb4py-0.2.0.dist-info/METADATA", "Name: fastdb4py\nVersion: 0.2.0\n")
                archive.writestr("fastdb4py/payload/__init__.py", "")
                archive.writestr("fastdb4py-0.2.0.dist-info/licenses/LICENSE", (release.ROOT / "LICENSE").read_bytes())
                archive.writestr("fastdb4py-0.2.0.dist-info/licenses/THIRD_PARTY_NOTICES.txt", (release.ROOT / "THIRD_PARTY_NOTICES.txt").read_bytes())
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            write_wheel(output, "fastdb4py-0.2.0-cp312-cp312-win_amd64.whl")
            with patch.object(release, "source_sha", return_value="a" * 40), patch.object(release, "version", return_value="0.2.0"):
                release.record_part(output, "wheels-windows", release.WINDOWS_TARGET)
            document = json.loads((output / "wheels-windows.json").read_text())
            self.assertEqual(document["artifacts"][0]["python_abi"], "cp312")
            self.assertEqual(document["artifacts"][0]["target"], release.WINDOWS_TARGET)
            (output / "wheels-windows.json").unlink()
            (output / "fastdb4py-0.2.0-cp312-cp312-win_amd64.whl").unlink()
            write_wheel(output, "fastdb4py-0.2.0-cp312-cp312-manylinux_2_28_x86_64.whl")
            with patch.object(release, "source_sha", return_value="a" * 40), patch.object(release, "version", return_value="0.2.0"):
                with self.assertRaisesRegex(release.ReleaseError, "win_amd64"):
                    release.record_part(output, "wheels-windows", release.WINDOWS_TARGET)

    def test_manifest_rejects_mistagged_or_missing_windows_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            document = self.candidate(root)
            wheels = [record for record in document["artifacts"] if record["kind"] == "python-wheel"]
            wheels[0]["target"] = release.TARGETS[1]
            release.write_json(root / "release-manifest.json", document)
            with self.assertRaisesRegex(release.ReleaseError, "Mistagged"):
                release.verify(root, "a" * 40)
            document = self.candidate(root)
            document["artifacts"] = [
                record
                for record in document["artifacts"]
                if not (record["kind"] == "core-bundle" and record["target"] == release.WINDOWS_TARGET)
            ]
            release.write_json(root / "release-manifest.json", document)
            with self.assertRaisesRegex(release.ReleaseError, "Incomplete fixed"):
                release.verify(root, "a" * 40)
            document = self.candidate(root)
            document["artifacts"] = [
                record
                for record in document["artifacts"]
                if record["kind"] != "python-wheel" or record["target"] != release.WINDOWS_TARGET or record["python_abi"] != "cp314t"
            ]
            release.write_json(root / "release-manifest.json", document)
            with self.assertRaisesRegex(release.ReleaseError, "Incomplete Python ABI"):
                release.verify(root, "a" * 40)


if __name__ == "__main__":
    unittest.main()
