"""Owned error projection for the FastDB payload ABI."""

from __future__ import annotations


class PayloadError(Exception):
    """A fully owned copy of a structured FastDB Core error."""

    def __init__(
        self,
        code: int,
        symbol: str,
        path: str,
        message: str,
        details_json: str,
    ) -> None:
        self.code = code
        self.symbol = symbol
        self.path = path
        self.message = message
        self.details_json = details_json
        super().__init__(f"{symbol} ({code}) at {path}: {message}")


def binding_error(
    message: str,
    *,
    path: str = "",
    reason: str = "binding_contract",
) -> PayloadError:
    return PayloadError(
        9001,
        "BINDING_CONTRACT",
        path,
        message,
        f'{{"reason":"{reason}"}}',
    )
