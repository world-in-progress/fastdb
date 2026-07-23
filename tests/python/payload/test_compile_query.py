from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from fastdb4py.payload import Capabilities, CompiledSpec, PayloadError, Profile


ROOT = Path(__file__).resolve().parents[3]
SPEC_GOLDEN = ROOT / "tests" / "golden" / "payload" / "v1" / "spec"


def _bytes_from_hex(path: Path) -> bytes:
    return bytes.fromhex(path.read_text(encoding="ascii"))


def test_compile_and_query_record_all_types_through_core() -> None:
    source = (SPEC_GOLDEN / "valid" / "record-all-types.source.json").read_bytes()
    expected_digest = (
        SPEC_GOLDEN / "valid" / "record-all-types.sha256"
    ).read_text(encoding="ascii").strip()

    with CompiledSpec.compile(source) as spec:
        clone = spec.clone()
        try:
            assert spec.canonical_json() == _bytes_from_hex(
                SPEC_GOLDEN / "valid" / "record-all-types.canonical.hex"
            )
            assert spec.manifest_json() == _bytes_from_hex(
                SPEC_GOLDEN / "valid" / "record-all-types.manifest.hex"
            )
            assert spec.sha256().hex() == expected_digest
            assert spec.profile() is Profile.RECORD_V1
            assert spec.capabilities() == Capabilities(
                profile=Profile.RECORD_V1,
                semantic_flags=0x1B,
                operation_flags=0xFF,
                codegen_target_flags=0x0F,
                direct_build_status=1,
            )
            assert spec.entry_count() == 2
            assert spec.entry_id(0) == "single"
            assert spec.entry_id(1) == "series"
            assert spec.entry_index("series") == 1
            assert spec.component_count() == 2
            assert spec.component_id(0) == "AllTypes"
            assert spec.component_id(1) == "Leaf"
            assert spec.component_index("Leaf") == 1
            assert spec.component_field_count(0) == 14
            assert spec.component_field_id(0, 9) == "str_value"
            assert spec.component_field_index(0, "list_value") == 13
            assert clone.sha256() == spec.sha256()
        finally:
            clone.close()


def test_compile_error_preserves_every_core_field() -> None:
    source = (SPEC_GOLDEN / "invalid" / "bad-kind.source.json").read_bytes()
    with pytest.raises(PayloadError) as caught:
        CompiledSpec.compile(source)

    error = caught.value
    assert error.code == 1005
    assert error.symbol == "INVALID_TYPE"
    assert error.path == "/entries/0/type/items/kind"
    assert error.message == "Payload type kind is invalid"
    assert error.details_json == '{"actual":"text","reason":"invalid_type_kind"}'


def test_compiled_spec_clone_and_concurrent_queries_own_native_lifetimes() -> None:
    source = (SPEC_GOLDEN / "valid" / "record-all-types.source.json").read_bytes()
    original = CompiledSpec.compile(source)
    clone = original.clone()
    original.close()
    original.close()

    try:
        with ThreadPoolExecutor(max_workers=8) as executor:
            digests = list(executor.map(lambda _: clone.sha256(), range(64)))
        assert all(digest == digests[0] for digest in digests)
    finally:
        clone.close()
        clone.close()

    with pytest.raises(PayloadError, match="CompiledSpec is closed"):
        clone.sha256()
