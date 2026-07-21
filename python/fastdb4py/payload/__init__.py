"""Official Python projection of the FastDB portable-payload Core."""

from ._error import PayloadError
from ._spec import Capabilities, CompiledSpec, Profile

__all__ = ["Capabilities", "CompiledSpec", "PayloadError", "Profile"]
