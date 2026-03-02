from importlib.metadata import version as _pkg_version, PackageNotFoundError

from .transport import Transport
from .channels import Channels, ProtocolChannels

try:
    __version__ = _pkg_version("raccoon-transport")
except PackageNotFoundError:
    __version__ = "dev"
