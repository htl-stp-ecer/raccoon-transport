"""Public Python API for the shared Raccoon transport package.

This package exposes:

- ``Transport``: a transport wrapper with reliable and retained delivery helpers.
- ``Channels``: stable application-level channel names.
- ``ProtocolChannels``: internal channels used by the transport protocol.
"""

from importlib.metadata import PackageNotFoundError, version as _pkg_version

from ._contract import CONTRACT_HASH, CONTRACT_MARKER
from .channels import Channels, ProtocolChannels
from .transport import Transport

try:
    __version__ = _pkg_version("raccoon-transport")
except PackageNotFoundError:
    __version__ = "dev"

def contract_hash() -> str:
    """Contract hash compiled into this package (see ``scripts/contract.py``)."""
    return CONTRACT_HASH


def transport_version() -> str:
    """Transport release for display. The contract hash is what is compared."""
    return __version__


__all__ = [
    "Transport",
    "Channels",
    "ProtocolChannels",
    "CONTRACT_HASH",
    "CONTRACT_MARKER",
    "contract_hash",
    "transport_version",
    "__version__",
]
