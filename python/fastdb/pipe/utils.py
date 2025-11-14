import weakref
from threading import Lock
from typing import Dict, get_type_hints, get_origin, get_args

from .base import BasePipe
from ..type import OriginFieldType, get_origin_type

_cache_lock = Lock()
_global_pipe_defn_cache = weakref.WeakKeyDictionary()

def parse_defns(cls):
    if cls in _global_pipe_defn_cache:
        return _global_pipe_defn_cache[cls]
    
    with _cache_lock:
        if cls in _global_pipe_defn_cache:
            return _global_pipe_defn_cache[cls]
        
        m: Dict[str, tuple[OriginFieldType, int]] = {}
        hints = get_type_hints(cls)
        for idx, (field_name, hint) in enumerate(hints.items()):
            if field_name.startswith('_'):
                continue
            
            try:
                origin_type = get_origin_type(hint)
                if origin_type == OriginFieldType.unknown:
                    if issubclass(hint, BasePipe):
                        origin_type = OriginFieldType.ref
            except Exception as e:
                origin_type = OriginFieldType.unknown
            m[field_name] = (origin_type, idx)
        
        _global_pipe_defn_cache[cls] = m
        return m

def get_defn(cls, field_name) -> tuple[OriginFieldType, int] | None:
    m = parse_defns(cls)
    return m.get(field_name)

def get_all_defns(cls) -> list[tuple[str, OriginFieldType]]:
    m = parse_defns(cls)
    # Return sorted list of definitions by field index
    return [(field_name, defn[0]) for field_name, defn in sorted(m.items(), key=lambda item: item[1][1])]