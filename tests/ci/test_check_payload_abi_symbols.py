#!/usr/bin/env python3
"""Unit tests for the payload ABI symbol checker (dumpbin and allowlist)."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = (
    Path(__file__).resolve().parents[2] / "tools" / "check_payload_abi_symbols.py"
)
SPEC = importlib.util.spec_from_file_location("check_payload_abi_symbols", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


DUMPBIN_OUTPUT = "\r\n".join(
    [
        "Microsoft (R) COFF/PE Dumper Version 14.42.34435.0",
        "Copyright (C) Microsoft Corporation.  All rights reserved.",
        "",
        "",
        "Dump of file C:\\b\\release\\fastcarto\\fastdb\\fastdb.dll",
        "",
        "File Type: DLL",
        "",
        "  Section contains the following exports for fastdb.dll",
        "",
        "    00000000 characteristics",
        "    FFFFFFFF version",
        "           3 number of functions",
        "           2 number of names",
        "",
        "    ordinal hint RVA      name",
        "",
        "          1    0 0002B4F0 fdb_payload_v1_abi_version",
        "          2    1 0003A610 fdb_payload_v1_access_bytes",
        "          3    2 0003A640",
        "",
        "  Summary",
        "",
        "        1000 .data",
        "        2000 .rdata",
        "        3000 .reloc",
    ]
)


class DumpbinExportParsingTests(unittest.TestCase):
    def test_extracts_exact_named_exports_and_ignores_surroundings(self) -> None:
        self.assertEqual(
            MODULE.parse_dumpbin_exports(DUMPBIN_OUTPUT),
            ["fdb_payload_v1_abi_version", "fdb_payload_v1_access_bytes"],
        )

    def test_requires_exactly_one_export_section_header(self) -> None:
        for text in (
            DUMPBIN_OUTPUT.replace(
                "  Section contains the following exports for fastdb.dll", ""
            ),
            DUMPBIN_OUTPUT.replace(
                "  Summary",
                "  Section contains the following exports for fastdb.dll",
            ),
        ):
            with self.subTest(text=text[-120:]), self.assertRaises(MODULE.CheckError):
                MODULE.parse_dumpbin_exports(text)

    def test_rejects_payload_named_lines_the_row_parser_cannot_accept(self) -> None:
        forwarded = DUMPBIN_OUTPUT.replace(
            "          3    2 0003A640",
            "          3    2 0003A640 fdb_payload_v1_extra "
            "(forwarded to other.dll.fdb_payload_v1_extra)",
        )
        with self.assertRaisesRegex(MODULE.CheckError, "row parser rejects"):
            MODULE.parse_dumpbin_exports(forwarded)


class AllowlistContractTests(unittest.TestCase):
    def test_reviewed_allowlist_is_exact_117_sorted_symbols(self) -> None:
        symbols = MODULE.read_allowlist(MODULE.ALLOWLIST)
        self.assertEqual(len(symbols), 117)
        self.assertEqual(symbols, sorted(set(symbols)))


if __name__ == "__main__":
    unittest.main()
