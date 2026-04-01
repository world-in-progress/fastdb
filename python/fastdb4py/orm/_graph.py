from __future__ import annotations
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..feature.feature import Feature
from ..type import OriginFieldType


class _GraphCollector:
    """
    Two-pass DFS collector for Feature object graphs.

    Traverses the Feature graph starting at a root, assigning every reachable
    Feature a (layer_name, idx) in post-order (leaves first). Detects back-edges
    (cycles) via an on-stack set and records them for later patching.
    """

    def __init__(self):
        self.id_map:     dict = {}   # id(feat) → (layer_name, idx)
        self.order:      list = []   # post-order traversal list
        self.back_edges: set  = set() # (id(feat), field_name) for cyclic refs
        self._visiting:  set  = set()
        self._layer_counters: dict = {}

    def collect(self, feature, _parent_id=None, _parent_field=None):
        if feature is None:
            return
        fid = id(feature)
        if fid in self.id_map:
            return
        if fid in self._visiting:
            if _parent_id is not None and _parent_field is not None:
                self.back_edges.add((_parent_id, _parent_field))
            return

        self._visiting.add(fid)

        hints  = getattr(feature, '_origin_hints', {})
        schema = getattr(feature, '_schema', None)

        for name, (ft, _) in hints.items():
            if ft == OriginFieldType.ref:
                child = feature._cache.get(name)
                if child is not None and hasattr(child, '_origin_hints'):
                    self.collect(child, _parent_id=fid, _parent_field=name)
            elif ft == OriginFieldType.list:
                if schema and schema.list_element_types.get(name) == OriginFieldType.ref:
                    for child in (feature._cache.get(name) or []):
                        if child is not None and hasattr(child, '_origin_hints'):
                            self.collect(child, _parent_id=fid, _parent_field=name)

        self._visiting.discard(fid)
        layer_name = type(feature).__name__
        idx = self._layer_counters.get(layer_name, 0)
        self._layer_counters[layer_name] = idx + 1
        self.id_map[fid] = (layer_name, idx)
        self.order.append(feature)
