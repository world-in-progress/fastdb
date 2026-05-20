# python/fastdb4py/push_compiler.py
"""Shared push compilation infrastructure for both old ORM and ORM2.

Extracted from feature/_schema.py to avoid coupling ORM2 to the Feature subclass system.
"""
import struct as _struct
import functools as _ft

from .core import _fastdb4py as _fdb_c

# Direct C extension function references — bypass SWIG wrapper (~200ns/call savings)
_c_add_begin  = _fdb_c.WxLayerTableBuild_add_feature_begin
_c_add_end    = _fdb_c.WxLayerTableBuild_add_feature_end
_c_set_field  = _fdb_c.WxLayerTableBuild_set_field
_c_set_cstr   = _fdb_c.WxLayerTableBuild_set_field_cstring
_c_set_wstr   = _fdb_c.WxLayerTableBuild_set_field_wstring
_c_set_raw    = _fdb_c.WxLayerTableBuild_set_geometry_raw
_c_set_list   = _fdb_c.WxLayerTableBuild_set_field_list_numeric
_c_push_dict  = _fdb_c.WxLayerTableBuild_push_from_dict
_c_pfd_fc     = _fdb_c.WxLayerTableBuild_push_from_dict_fc
# Batch variant: processes a list of cache dicts in one C call (registered after SWIG rebuild)
_c_pmfd_fc    = getattr(_fdb_c, 'WxLayerTableBuild_push_many_from_dicts_fc', None)


def compile_push_fn(numeric_plan, str_plan, bytes_plan, list_plan):
    """Generate and compile a specialized per-class push function.

    The compiled function signature is:
        push_fn(cache, t) -> None

    For list fields, uses per-field int-keyed dicts (baked into exec namespace)
    for fast struct.Struct pack-method lookup (avoids function call overhead and
    tuple key creation compared to the _get_struct_pack_method approach).
    """
    lines = ['def _push(cache, t, _ab=_c_add_begin, _ae=_c_add_end, _sf=_c_set_field, _sfc=_c_set_cstr, _sfw=_c_set_wstr, _sr=_c_set_raw, _sl=_c_set_list):']
    lines.append('    _ab(t)')
    for idx, fn in numeric_plan:
        lines.append(f'    _sf(t, {idx}, cache.get({fn!r}) or 0)')
    for idx, fn, is_wide in str_plan:
        if is_wide:
            lines.append(f'    _sfw(t, {idx}, cache.get({fn!r}) or "")')
        else:
            lines.append(f'    _sfc(t, {idx}, cache.get({fn!r}) or "")')
    for idx, fn in bytes_plan:
        lines.append(f'    _sr(t, cache.get({fn!r}) or b"")')
    for i, (idx, fn, typecode) in enumerate(list_plan):
        gv = f'_gsp{i}'
        lines.append(f'    _items = cache.get({fn!r}) or []')
        lines.append(f'    _n = len(_items)')
        lines.append(f'    _sl(t, {idx}, ({gv}[_n] if _n in {gv} else {gv}.setdefault(_n, _SS(str(_n)+{typecode!r}).pack))(*_items))')
    lines.append('    _ae(t)')
    src = '\n'.join(lines)
    ns: dict = {f'_gsp{i}': {} for i in range(len(list_plan))}
    ns['_c_add_begin'] = _c_add_begin
    ns['_c_add_end'] = _c_add_end
    ns['_c_set_field'] = _c_set_field
    ns['_c_set_cstr'] = _c_set_cstr
    ns['_c_set_wstr'] = _c_set_wstr
    ns['_c_set_raw'] = _c_set_raw
    ns['_c_set_list'] = _c_set_list
    if list_plan:
        ns['_SS'] = _struct.Struct
    exec(compile(src, '<push_fn>', 'exec'), ns)
    return ns['_push']


def compile_ref_push_fn(numeric_plan, str_plan, bytes_plan, list_plan,
                        ref_plan, list_ref_plan):
    """Generate a push function that handles pre-resolved REF fields.

    Like compile_push_fn but with two extra plan types:
      - ref_plan: List[(field_id, field_name)]
          Scalar REF values have been resolved to WxFeatureRef ints before calling.
          Generated code: _sf(t, idx, cache.get(name) or 0)
      - list_ref_plan: List[(field_id, field_name)]
          LIST[REF] values have been pre-packed to raw bytes before calling.
          Generated code: _sl(t, idx, cache.get(name) or b"")
    """
    lines = ['def _push(cache, t, _ab=_c_add_begin, _ae=_c_add_end, _sf=_c_set_field, _sfc=_c_set_cstr, _sfw=_c_set_wstr, _sr=_c_set_raw, _sl=_c_set_list):']
    lines.append('    _ab(t)')
    for idx, fn in numeric_plan:
        lines.append(f'    _sf(t, {idx}, cache.get({fn!r}) or 0)')
    for idx, fn, is_wide in str_plan:
        if is_wide:
            lines.append(f'    _sfw(t, {idx}, cache.get({fn!r}) or "")')
        else:
            lines.append(f'    _sfc(t, {idx}, cache.get({fn!r}) or "")')
    for idx, fn in bytes_plan:
        lines.append(f'    _sr(t, cache.get({fn!r}) or b"")')
    for i, (idx, fn, typecode) in enumerate(list_plan):
        gv = f'_gsp{i}'
        lines.append(f'    _items = cache.get({fn!r}) or []')
        lines.append(f'    _n = len(_items)')
        lines.append(f'    _sl(t, {idx}, ({gv}[_n] if _n in {gv} else {gv}.setdefault(_n, _SS(str(_n)+{typecode!r}).pack))(*_items))')
    # Scalar REF fields — value already resolved to int
    for idx, fn in ref_plan:
        lines.append(f'    _sf(t, {idx}, cache.get({fn!r}) or 0)')
    # LIST[REF] fields — value already packed as raw bytes
    for idx, fn in list_ref_plan:
        lines.append(f'    _sl(t, {idx}, cache.get({fn!r}) or b"")')
    lines.append('    _ae(t)')
    src = '\n'.join(lines)
    ns: dict = {f'_gsp{i}': {} for i in range(len(list_plan))}
    ns['_c_add_begin'] = _c_add_begin
    ns['_c_add_end'] = _c_add_end
    ns['_c_set_field'] = _c_set_field
    ns['_c_set_cstr'] = _c_set_cstr
    ns['_c_set_wstr'] = _c_set_wstr
    ns['_c_set_raw'] = _c_set_raw
    ns['_c_set_list'] = _c_set_list
    if list_plan:
        ns['_SS'] = _struct.Struct
    exec(compile(src, '<ref_push_fn>', 'exec'), ns)
    return ns['_push']


def make_inlined_dispatch(numeric_plan, str_plan, bytes_plan, list_plan, t_obj,
                          pfd_num_names=None, pfd_num_ids=None,
                          pfd_str_names=None, pfd_str_ids=None):
    """Generate a per-(class, table) inlined push+dispatch function.

    For simple features (numeric + cstring str only, no bytes/list/wstr):
    uses push_from_dict which eliminates per-field SWIG overhead in one C call.

    For complex features (wstr, bytes, list): falls back to per-field C extension calls.

    This is used exclusively in the push() fast path. push_many() continues to
    use push_fn (which takes (cache, t_origin)) for its tight inner loop.
    """
    t_origin = t_obj._origin

    # Fast path: use push_from_dict (1 Python→C call) for simple numeric+cstring features
    use_pfd = (
        not bytes_plan and not list_plan and
        all(not is_wide for _, _, is_wide in str_plan) and
        pfd_num_names is not None
    )

    if use_pfd:
        # Use functools.partial to avoid a Python frame: dispatch_fn(cache) calls C directly.
        # push_from_dict_fc has cache as last arg so partial can pre-fill all other args.
        return _ft.partial(_c_pfd_fc, t_origin,
                           pfd_num_names, pfd_num_ids,
                           pfd_str_names, pfd_str_ids,
                           t_obj._fc)

    # Fallback: per-field C extension calls.
    lines = ['def _dispatch(cache, _ab=_c_ab, _ae=_c_ae, _sf=_c_sf, _sfc=_c_sfc, _to=to, _t=t_obj, _SS=None):']
    lines.append('    _ab(_to)')
    for idx, fn in numeric_plan:
        lines.append(f'    _sf(_to, {idx}, cache.get({fn!r}) or 0)')
    for idx, fn, is_wide in str_plan:
        if is_wide:
            lines.append(f'    _c_wstr(_to, {idx}, cache.get({fn!r}) or "")')
        else:
            lines.append(f'    _sfc(_to, {idx}, cache.get({fn!r}) or "")')
    for idx, fn in bytes_plan:
        lines.append(f'    _c_raw(_to, cache.get({fn!r}) or b"")')
    for i, (idx, fn, typecode) in enumerate(list_plan):
        gv = f'_gsp{i}'
        lines.append(f'    _items = cache.get({fn!r}) or []')
        lines.append(f'    _n = len(_items)')
        lines.append(f'    _c_list(_to, {idx}, ({gv}[_n] if _n in {gv} else {gv}.setdefault(_n, _SS(str(_n)+{typecode!r}).pack))(*_items))')
    lines.append('    _ae(_to)')
    lines.append('    _t.feature_count += 1')
    src = '\n'.join(lines)
    ns: dict = {
        '_c_ab': _c_add_begin, '_c_ae': _c_add_end,
        '_c_sf': _c_set_field, '_c_sfc': _c_set_cstr,
        '_c_wstr': _c_set_wstr, '_c_raw': _c_set_raw, '_c_list': _c_set_list,
        'to': t_origin, 't_obj': t_obj,
        **{f'_gsp{i}': {} for i in range(len(list_plan))},
    }
    if list_plan:
        ns['_SS'] = _struct.Struct
    exec(compile(src, '<inlined_dispatch>', 'exec'), ns)
    return ns['_dispatch']


def make_batch_inlined_dispatch(numeric_plan, str_plan, bytes_plan, list_plan, t_obj,
                                pfd_num_names=None, pfd_num_ids=None,
                                pfd_str_names=None, pfd_str_ids=None):
    """Like make_inlined_dispatch but returns a function that accepts a LIST of cache dicts.

    Only available for simple features (no bytes/list/wstr) and only when
    push_many_from_dicts_fc is present in the C extension.

    Returns None if the batch path is unavailable (falls back to single dispatch).
    """
    if _c_pmfd_fc is None:
        return None
    use_pfd = (
        not bytes_plan and not list_plan and
        all(not is_wide for _, _, is_wide in str_plan) and
        pfd_num_names is not None
    )
    if not use_pfd:
        return None
    return _ft.partial(_c_pmfd_fc, t_obj._origin,
                        pfd_num_names, pfd_num_ids,
                        pfd_str_names, pfd_str_ids,
                        t_obj._fc)
