# python/fastdb4py/layout.py
"""Layout: describes a pre-allocated table for an engine."""
from dataclasses import dataclass
from typing import Type
from .registry import is_feature


@dataclass(frozen=True)
class Layout:
    """Pre-allocation descriptor for engine.truncate().

    Args:
        feature_type: A @feature-decorated class.
        capacity: Number of rows to pre-allocate.
    """
    feature_type: Type
    capacity: int

    def __post_init__(self):
        if not is_feature(self.feature_type):
            raise TypeError(
                f"{self.feature_type!r} is not a @feature class. "
                f"Use @feature decorator. (is_feature check failed)"
            )
        if self.capacity < 0:
            raise ValueError(f"capacity must be non-negative, got {self.capacity}")
