"""Feature subpackage — @feature decorator re-export for convenience."""
from ..decorator import feature
from ..registry import is_feature

__all__ = ['feature', 'is_feature']
