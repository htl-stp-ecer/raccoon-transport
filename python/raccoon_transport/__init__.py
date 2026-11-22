"""The shared transport of the Raccoon platform.

- ``Transport``: publish and subscribe on the shared-memory rings (native).
- ``get_transport()``: the transport of this process, shared by every user.
- ``channels``: the names of all channels, generated from ``channels.yml``.
- ``contract_hash()``: the identifier of the wire format this package speaks.

Without the native module (a laptop install) the channel names, the message
classes and the contract check still work; ``Transport`` raises on use.
"""

from importlib.metadata import PackageNotFoundError, version as _pkg_version

from ._contract import CONTRACT_HASH, CONTRACT_MARKER
from .channel_table import channels

try:
    from ._native import PublishError, PublishResult, Subscription, Transport  # noqa: F401

    NATIVE = True
except ImportError:  # pure wheel
    NATIVE = False

    class PublishError(RuntimeError):  # type: ignore[no-redef]
        pass

    class Transport:  # type: ignore[no-redef]
        """Placeholder: this install has no native transport."""

        def __init__(self, *args, **kwargs):
            raise RuntimeError("raccoon_transport was installed without its native module")

try:
    __version__ = _pkg_version("raccoon-transport")
except PackageNotFoundError:
    __version__ = "dev"

_transport = None


def get_transport() -> "Transport":
    """The transport of this process. It is created on first use and again after
    ``shutdown_transport()``."""
    global _transport
    if _transport is None or not _transport.is_alive():
        _transport = Transport()
        _install_exit_hook()
    return _transport


def shutdown_transport() -> None:
    """Stop the transport of this process. A later ``get_transport()`` makes a new one."""
    global _transport
    if _transport is not None:
        _transport.shutdown()
        _transport = None


_hook_installed = False


def _install_exit_hook() -> None:
    # An orderly shutdown while the interpreter is still whole, then _exit(): the
    # rest of the interpreter teardown and the static destructors of the native
    # code are skipped, because a thread of the transport may still hold the GIL.
    global _hook_installed
    if _hook_installed:
        return
    _hook_installed = True
    import atexit
    import os
    import sys

    def finalize():
        try:
            shutdown_transport()
        except Exception:
            pass
        if os.environ.get("RACCOON_TRANSPORT_NO_EXIT_HOOK") == "1":
            return  # a test runner keeps its own exit status
        # A crash stays visible in the exit status; Ctrl+C is a clean exit.
        crash = getattr(sys, "last_exc", None)
        failed = crash is not None and not isinstance(crash, KeyboardInterrupt)
        os._exit(1 if failed else 0)

    atexit.register(finalize)


def contract_hash() -> str:
    """Contract hash compiled into this package (see ``scripts/contract.py``)."""
    return CONTRACT_HASH


def transport_version() -> str:
    """Transport release for display. The contract hash is what is compared."""
    return __version__


__all__ = [
    "NATIVE",
    "Transport",
    "PublishError",
    "PublishResult",
    "Subscription",
    "channels",
    "get_transport",
    "shutdown_transport",
    "CONTRACT_HASH",
    "CONTRACT_MARKER",
    "contract_hash",
    "transport_version",
    "__version__",
]
