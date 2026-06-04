"""Public Python API for the shared Raccoon transport package.

This package exposes:

- ``Transport``: a transport wrapper with reliable and retained delivery helpers.
- ``Channels``: stable application-level channel names.
- ``ProtocolChannels``: internal channels used by the transport protocol.
"""

from importlib.metadata import PackageNotFoundError, version as _pkg_version

from .channels import Channels, ProtocolChannels
from .transport import Transport

try:
    __version__ = _pkg_version("raccoon-transport")
except PackageNotFoundError:
    __version__ = "dev"

__all__ = ["Transport", "Channels", "ProtocolChannels", "__version__"]
