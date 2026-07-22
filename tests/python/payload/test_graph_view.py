from __future__ import annotations

from pathlib import Path
import threading

from fastdb4py.payload import (
    BuildPolicy,
    Builder,
    CompiledSpec,
    GraphIdentity,
    Payload,
    PayloadError,
    View,
    ViewKind,
)


FIXTURES = Path(__file__).parents[2] / "golden/payload/v1/binary/spec"


def _compile(name: str) -> CompiledSpec:
    return CompiledSpec.compile((FIXTURES / name).read_bytes())


def graph_payload() -> tuple[Payload, int, int, int]:
    spec = _compile("graph-all-values.source.json")
    node_index = spec.component_index("Node")
    inline_index = spec.component_index("Inline")
    asset_index = spec.component_index("Asset")
    builder = Builder.create(spec)
    spec.close()
    try:
        node = builder.declare_object(node_index)
        asset = builder.declare_object(asset_index)
        (
            builder.object_fill_begin(node)
            .value_bool(True)
            .value_u8(0x12)
            .value_u16(0x3456)
            .value_u32(0x789A_BCDE)
            .value_i32(-1_234_567)
            .value_u8n_bits(0x3FE0_0000_0000_0000)
            .value_u16n_bits(0)
            .value_f32_bits(0x7FA1_2345)
            .value_f64_bits(0xFFF8_0000_0000_1234)
            .value_str("same")
            .value_wstr_units((0x0041, 0xD83D, 0xDE00))
            .value_bytes(b"\x00\xff~")
            .value_component_begin()
            .value_null()
            .value_u16(0xBEEF)
            .value_list_begin(3)
            .value_f32_bits(0x8000_0000)
            .value_null()
            .value_f32_bits(0xFF80_0001)
            .value_ref(node)
            .value_ref(asset)
            .object_fill_begin(asset)
            .value_str("same")
            .value_ref(node)
            .entry_begin(0, 1)
            .value_object(node)
            .entry_begin(1, 1)
            .value_object(asset)
            .entry_begin(2, 2)
            .value_ref(node)
            .value_null()
            .entry_begin(3, 3)
            .value_u8n_bits(0)
            .value_u8n_bits(0x3FE0_0000_0000_0000)
            .value_u8n_bits(0x3FF0_0000_0000_0000)
            .entry_begin(4, 3)
            .value_u16n_bits(0xBFF0_0000_0000_0000)
            .value_u16n_bits(0)
            .value_u16n_bits(0x3FF0_0000_0000_0000)
        )
        plan = builder.freeze()
    finally:
        builder.close()
    try:
        payload = plan.execute(BuildPolicy.ALLOW_STAGING).payload
    finally:
        plan.close()
    return payload, node_index, inline_index, asset_index


def root(payload: Payload, entry_index: int) -> View:
    with payload.entry_view(entry_index) as sequence:
        return sequence.at(0)


def require_error(
    error: PayloadError,
    code: int,
    symbol: str,
    path: str,
    message: str,
    details: str,
) -> None:
    assert error.code == code
    assert error.symbol == symbol
    assert error.path == path
    assert error.message == message
    assert error.details_json == details


def test_graph_identity_refs_cycles_and_materialized_closure_are_core_owned() -> None:
    payload, node_index, inline_index, asset_index = graph_payload()
    source_root = root(payload, 0)
    root_identity = GraphIdentity(node_index, 0)
    asset_identity = GraphIdentity(asset_index, 0)
    assert source_root.graph_identity() == root_identity

    with source_root.field(12) as inline:
        assert inline.component_index() == inline_index
        try:
            inline.graph_identity()
        except PayloadError as error:
            require_error(
                error,
                2004,
                "TYPE_MISMATCH",
                "/entries/0/0/fields/12",
                "Portable payload view kind does not match the operation",
                '{"reason":"view_kind_mismatch"}',
            )
        else:
            raise AssertionError("inline component unexpectedly has graph identity")

    self_ref = source_root.field(14)
    assert self_ref.kind() is ViewKind.REF
    assert self_ref.graph_identity() == root_identity
    try:
        self_ref.field(0)
    except PayloadError as error:
        require_error(
            error,
            2004,
            "TYPE_MISMATCH",
            "/entries/0/0/fields/14",
            "Portable payload view kind does not match the operation",
            '{"reason":"view_kind_mismatch"}',
        )
    else:
        raise AssertionError("ref view unexpectedly permits implicit field access")
    with self_ref.ref_target() as self_target:
        assert self_target.graph_identity() == root_identity

    asset_ref = source_root.field(15)
    assert asset_ref.graph_identity() == asset_identity
    with asset_ref.ref_target() as asset:
        assert asset.graph_identity() == asset_identity
        with asset.field(1) as owner_ref:
            assert owner_ref.graph_identity() == root_identity
            with owner_ref.ref_target() as owner:
                assert owner.graph_identity() == root_identity

    with payload.entry_view(2) as refs:
        with refs.at(0) as shared_ref:
            assert shared_ref.graph_identity() == root_identity
            with shared_ref.ref_target() as shared:
                assert shared.graph_identity() == root_identity
        with refs.at(1) as null_ref:
            assert null_ref.is_null()
            try:
                null_ref.ref_target()
            except PayloadError as error:
                require_error(
                    error,
                    2003,
                    "UNEXPECTED_NULL",
                    "/entries/2/1",
                    "Portable payload view is null",
                    '{"reason":"unexpected_null"}',
                )
            else:
                raise AssertionError("null ref unexpectedly has a target")
            try:
                null_ref.graph_identity()
            except PayloadError as error:
                require_error(
                    error,
                    2003,
                    "UNEXPECTED_NULL",
                    "/entries/2/1",
                    "Portable payload view is null",
                    '{"reason":"unexpected_null"}',
                )
            else:
                raise AssertionError("null ref unexpectedly has graph identity")

    failures: list[BaseException] = []
    workers: list[threading.Thread] = []
    for _ in range(4):
        worker_root = source_root.clone()
        worker_ref = asset_ref.clone()

        def traverse(local_root: View = worker_root, local_ref: View = worker_ref) -> None:
            try:
                for _ in range(250):
                    assert local_root.graph_identity().object_id == 0
                    with local_ref.ref_target() as target:
                        assert target.graph_identity().object_id == 0
            except BaseException as error:
                failures.append(error)
            finally:
                local_ref.close()
                local_root.close()

        worker = threading.Thread(target=traverse)
        worker.start()
        workers.append(worker)
    for worker in workers:
        worker.join(timeout=5)
        assert not worker.is_alive()
    assert failures == []

    detached = source_root.materialize()
    assert detached.graph_identity() == root_identity
    payload.invalidate()
    try:
        source_root.graph_identity()
    except PayloadError as error:
        require_error(
            error,
            4001,
            "VIEW_INVALIDATED",
            "/view",
            "Portable payload access barrier rejected the operation",
            '{"reason":"view_invalidated"}',
        )
    else:
        raise AssertionError("invalidated source root remained queryable")
    try:
        asset_ref.ref_target()
    except PayloadError as error:
        require_error(
            error,
            4001,
            "VIEW_INVALIDATED",
            "/view",
            "Portable payload access barrier rejected the operation",
            '{"reason":"view_invalidated"}',
        )
    else:
        raise AssertionError("invalidated source ref remained queryable")
    asset_ref.close()
    self_ref.close()
    source_root.close()
    payload.close()

    with detached.field(14) as detached_self:
        assert detached_self.graph_identity() == root_identity
        with detached_self.ref_target() as target:
            assert target.graph_identity() == root_identity
    with detached.field(15) as detached_asset_ref:
        with detached_asset_ref.ref_target() as detached_asset:
            assert detached_asset.graph_identity() == asset_identity
            with detached_asset.field(1) as owner_ref:
                with owner_ref.ref_target() as owner:
                    assert owner.graph_identity() == root_identity
    detached.close()


def test_disconnected_roots_have_distinct_payload_scoped_identities() -> None:
    spec = _compile("graph-disconnected-roots.source.json")
    component = spec.component_index("Node")
    builder = Builder.create(spec)
    spec.close()
    try:
        first_handle = builder.declare_object(component)
        second_handle = builder.declare_object(component)
        (
            builder.object_fill_begin(first_handle)
            .value_u32(11)
            .value_null()
            .object_fill_begin(second_handle)
            .value_u32(22)
            .value_null()
            .entry_begin(0, 2)
            .value_object(first_handle)
            .value_object(second_handle)
        )
        plan = builder.freeze()
    finally:
        builder.close()
    try:
        payload = plan.execute(BuildPolicy.ALLOW_STAGING).payload
    finally:
        plan.close()

    with payload.entry_view(0) as roots:
        with roots.at(0) as first:
            assert first.graph_identity() == GraphIdentity(component, 0)
            with first.field(0) as value:
                assert value.get_u32() == 11
        with roots.at(1) as second:
            assert second.graph_identity() == GraphIdentity(component, 1)
            with second.field(0) as value:
                assert value.get_u32() == 22

    independent, _, _, _ = graph_payload()
    with root(independent, 0) as independent_root:
        assert independent_root.graph_identity().object_id == 0
    payload.invalidate()
    payload.close()
    with root(independent, 0) as independent_root:
        with independent_root.field(3) as value:
            assert value.get_u32() == 0x789A_BCDE
    independent.close()
