"""Official Python projection of the FastDB portable-payload Core."""

from ._error import PayloadError
from ._builder import (
    BuildPlan,
    Builder,
    BuilderOptions,
    FixedRun,
    ObjectHandle,
    PlanInfo,
)
from ._spec import Capabilities, CompiledSpec, Profile
from ._runtime import (
    BackingStatus,
    BuildPolicy,
    BuildResult,
    ExecutionMode,
    ExecutionReport,
    ExternalBytes,
    FallbackReason,
    MemoryBacking,
    OpenOptions,
    Payload,
    ReserveMode,
)

__all__ = [
    "BuildPlan",
    "BackingStatus",
    "BuildPolicy",
    "BuildResult",
    "Builder",
    "BuilderOptions",
    "Capabilities",
    "CompiledSpec",
    "FixedRun",
    "ExecutionMode",
    "ExecutionReport",
    "ExternalBytes",
    "FallbackReason",
    "MemoryBacking",
    "ObjectHandle",
    "OpenOptions",
    "Payload",
    "PayloadError",
    "PlanInfo",
    "Profile",
    "ReserveMode",
]
