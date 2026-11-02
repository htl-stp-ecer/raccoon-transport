"""Startup check of the transport contract. Semantics: ``contract/CHECK_SPEC.md``.

Every component runs :func:`startup_check` once when it starts. The result goes
to a status file (never over the transport, which may be the thing that is
broken) and, for ``mismatch`` and ``bundle-fehler``, the component refuses to
start. The Pi server reads the status files with :func:`aggregate`.
"""
from __future__ import annotations

import json
import logging
import os
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from importlib.metadata import PackageNotFoundError, version as _pkg_version
from pathlib import Path
from typing import Any, Optional

from ._contract import CONTRACT_HASH

log = logging.getLogger("raccoon_transport.contract")

#: Reserved exit code of a component that refuses to start. systemd units set
#: ``RestartPreventExitStatus=78`` so a refusal is not restarted in a loop.
EXIT_CODE = 78

DEFAULT_BUNDLE_FILE = "/etc/raccoon-bundle.json"
DEFAULT_DEVICE_MARKER = "/etc/raccoon-image"
DEFAULT_STATUS_DIR = "/run/raccoon"

OK = "ok"
MISMATCH = "mismatch"
DEV_WARNING = "dev-warnung"
BUNDLE_ERROR = "bundle-fehler"
VERSION_MISMATCH = "version-mismatch"
SKIPPED = "skipped"
REFUSING = (MISMATCH, BUNDLE_ERROR, VERSION_MISMATCH)

_REASON_TEXT = {
    "file-missing": "bundle file is missing",
    "file-unreadable": "bundle file is unreadable",
    "field-missing": "bundle file has no contract_hash field",
    "field-empty": "bundle file has an empty contract_hash",
}
_VERSION_REASONS = ("version-field-missing", "version-field-empty")


class ContractError(RuntimeError):
    """The transport contract of this component does not match the bundle."""

    def __init__(self, result: "CheckResult"):
        super().__init__(result.message)
        self.result = result


@dataclass
class CheckResult:
    component: str
    component_version: str
    own_hash: str
    own_transport_version: str
    expected_hash: Optional[str]
    expected_transport_version: Optional[str]
    bundle: Optional[str]
    result: str
    reason: Optional[str]
    message: str
    bundle_file: str = DEFAULT_BUNDLE_FILE
    bundle_key: str = ""
    expected_component_version: Optional[str] = None

    @property
    def refuse(self) -> bool:
        return self.result in REFUSING


def own_transport_version() -> str:
    try:
        return _pkg_version("raccoon-transport")
    except PackageNotFoundError:
        return "dev"


def _name(value: Optional[str]) -> str:
    return value if value else "unknown"


def _normalize_version(value: Optional[str]) -> str:
    """`v173` and `173` are the same version; nothing else is normalised."""
    value = (value or "").strip()
    return value[1:] if value[:1] in ("v", "V") else value


def _message(r: CheckResult) -> str:
    if r.result == OK:
        return f"contract ok: {r.own_hash}"
    if r.result == SKIPPED:
        return "not running on a raccoon device; contract check skipped"
    if r.result == DEV_WARNING and r.reason == "version-omitted":
        return (
            f"component version check omitted ('{r.bundle_key}' is 'dev' in the bundle): "
            f"{r.component} {_name(r.component_version)} is not compared; mismatches are not enforced"
        )
    if r.result == DEV_WARNING:
        return (
            "transport contract check is in dev mode (bundle field is 'dev'): "
            f"{r.component} uses contract {r.own_hash} (transport {_name(r.own_transport_version)}); "
            "mismatches are not enforced"
        )
    if r.result == MISMATCH:
        return (
            f"transport contract mismatch: {r.component} {_name(r.component_version)} uses contract "
            f"{r.own_hash} (transport {_name(r.own_transport_version)}), bundle {_name(r.bundle)} expects "
            f"{r.expected_hash} (transport {_name(r.expected_transport_version)}); "
            "update or reflash so all components match"
        )
    if r.result == VERSION_MISMATCH:
        return (
            f"component version mismatch: {r.component} is {_name(r.component_version)} but bundle "
            f"{_name(r.bundle)} expects {r.expected_component_version}; update or reflash so all components match"
        )
    if r.reason in _VERSION_REASONS:
        what = "no version" if r.reason == "version-field-missing" else "an empty version"
        return f"bundle check failed: bundle file has {what} for {r.bundle_key}: {r.bundle_file}"
    return f"transport contract check failed: {_REASON_TEXT[r.reason or 'file-unreadable']}: {r.bundle_file}"


def check(
    component: str,
    component_version: str,
    *,
    bundle_key: Optional[str] = None,
    own_hash: Optional[str] = None,
    own_version: Optional[str] = None,
    bundle_file: Optional[str] = None,
    device_marker: Optional[str] = None,
    force: Optional[bool] = None,
) -> CheckResult:
    """Run the decision of the spec. Reads one file, writes nothing."""
    bundle_file = bundle_file or os.environ.get("RACCOON_BUNDLE_FILE") or DEFAULT_BUNDLE_FILE
    device_marker = device_marker or os.environ.get("RACCOON_DEVICE_MARKER") or DEFAULT_DEVICE_MARKER
    if force is None:
        force = os.environ.get("RACCOON_CONTRACT_CHECK") == "force"
    result = CheckResult(
        component=component,
        component_version=component_version,
        own_hash=own_hash or CONTRACT_HASH,
        own_transport_version=own_version or own_transport_version(),
        expected_hash=None,
        expected_transport_version=None,
        bundle=None,
        result=SKIPPED,
        reason=None,
        message="",
        bundle_file=bundle_file,
        bundle_key=bundle_key or component,
    )

    def done(outcome: str, reason: Optional[str] = None) -> CheckResult:
        result.result = outcome
        result.reason = reason
        result.message = _message(result)
        return result

    if not force and not Path(device_marker).is_dir():
        return done(SKIPPED)

    try:
        text = Path(bundle_file).read_text(encoding="utf-8")
    except FileNotFoundError:
        return done(BUNDLE_ERROR, "file-missing")
    except (OSError, UnicodeDecodeError):
        return done(BUNDLE_ERROR, "file-unreadable")
    try:
        document = json.loads(text)
    except ValueError:
        return done(BUNDLE_ERROR, "file-unreadable")
    if not isinstance(document, dict):
        return done(BUNDLE_ERROR, "file-unreadable")

    bundle = document.get("bundle")
    result.bundle = bundle if isinstance(bundle, str) else None
    components = document.get("components")
    if isinstance(components, dict) and isinstance(components.get("raccoon-transport"), str):
        result.expected_transport_version = components["raccoon-transport"]

    expected = document.get("contract_hash")
    if not isinstance(expected, str):
        return done(BUNDLE_ERROR, "field-missing")
    expected = expected.strip()
    if not expected:
        return done(BUNDLE_ERROR, "field-empty")
    result.expected_hash = expected
    contract_dev = expected == "dev"
    if not contract_dev and expected != result.own_hash:
        return done(MISMATCH)

    # The component's own version against the bundle's entry for it. Independent of
    # the contract: a dev contract does not excuse a wrong version, and 'dev' in the
    # component's own field omits only this comparison.
    entry = components.get(result.bundle_key) if isinstance(components, dict) else None
    if not isinstance(entry, str):
        return done(BUNDLE_ERROR, "version-field-missing")
    entry = entry.strip()
    if not entry:
        return done(BUNDLE_ERROR, "version-field-empty")
    result.expected_component_version = entry
    version_omitted = entry == "dev"
    if not version_omitted and _normalize_version(entry) != _normalize_version(component_version):
        return done(VERSION_MISMATCH)
    if contract_dev:
        return done(DEV_WARNING)
    if version_omitted:
        return done(DEV_WARNING, "version-omitted")
    return done(OK)


# ── Status files ─────────────────────────────────────────────────────────


def status_dir(override: Optional[str] = None) -> Path:
    return Path(override or os.environ.get("RACCOON_STATUS_DIR") or DEFAULT_STATUS_DIR)


def _pid_starttime(pid: int) -> Optional[int]:
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    # Field 2 (comm) may contain spaces and parentheses; split after the last ')'.
    fields = stat[stat.rindex(")") + 2 :].split()
    try:
        return int(fields[19])  # field 22 overall
    except (IndexError, ValueError):
        return None


def write_status(result: CheckResult, *, persistent: bool, directory: Optional[str] = None) -> Optional[Path]:
    """Write ``<component>.json`` atomically. Returns None (and logs) on failure."""
    target_dir = status_dir(directory)
    pid = os.getpid()
    document: dict[str, Any] = {
        "component": result.component,
        "component_version": result.component_version,
        "own_hash": result.own_hash,
        "own_transport_version": result.own_transport_version,
        "expected_hash": result.expected_hash,
        "expected_transport_version": result.expected_transport_version,
        "expected_component_version": result.expected_component_version,
        "bundle": result.bundle,
        "result": result.result,
        "reason": result.reason,
        "message": result.message,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "pid": pid,
        "pid_starttime": _pid_starttime(pid),
        "persistent": persistent,
    }
    try:
        target_dir.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix=f".{result.component}.", suffix=".tmp", dir=target_dir)
        try:
            with os.fdopen(fd, "w") as out:
                json.dump(document, out, indent=2)
                out.write("\n")
            os.chmod(tmp, 0o644)
            final = target_dir / f"{result.component}.json"
            os.replace(tmp, final)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise
        return final
    except OSError as error:
        log.warning("could not write contract status file in %s: %s", target_dir, error)
        return None


def startup_check(
    component: str,
    component_version: str,
    *,
    persistent: bool,
    enforce: bool = True,
    status_directory: Optional[str] = None,
    **kwargs: Any,
) -> CheckResult:
    """Check, record, log. Raises :class:`ContractError` if the component must refuse."""
    result = check(component, component_version, **kwargs)
    if result.result != SKIPPED:
        write_status(result, persistent=persistent, directory=status_directory)
    if result.refuse:
        log.error("%s", result.message)
    elif result.result == DEV_WARNING:
        log.warning("%s", result.message)
    else:
        log.info("%s", result.message)
    if enforce and result.refuse:
        raise ContractError(result)
    return result


def exit_if_refused(result: CheckResult) -> None:
    """For long-running services: exit with the reserved code."""
    if result.refuse:
        print(result.message, file=sys.stderr)
        raise SystemExit(EXIT_CODE)


# ── Aggregation for the Pi server ────────────────────────────────────────


def _active(document: dict[str, Any]) -> bool:
    pid = document.get("pid")
    if not isinstance(pid, int) or pid <= 0:
        return False
    started = document.get("pid_starttime")
    if started is None:
        return Path(f"/proc/{pid}").exists() if Path("/proc").is_dir() else False
    return _pid_starttime(pid) == started


def read_statuses(directory: Optional[str] = None) -> list[dict[str, Any]]:
    """All status files with an ``active`` flag. Unreadable files are reported, not hidden."""
    target = status_dir(directory)
    entries: list[dict[str, Any]] = []
    if not target.is_dir():
        return entries
    for path in sorted(target.glob("*.json")):
        try:
            document = json.loads(path.read_text())
            if not isinstance(document, dict):
                raise ValueError("not an object")
        except (OSError, ValueError) as error:
            entries.append(
                {
                    "component": path.stem,
                    "result": BUNDLE_ERROR,
                    "reason": "status-unreadable",
                    "message": f"status file {path} is unreadable: {error}",
                    "active": False,
                    "persistent": True,
                }
            )
            continue
        document["active"] = _active(document)
        entries.append(document)
    return entries


def on_device(device_marker: Optional[str] = None, force: Optional[bool] = None) -> bool:
    """True on a Raccoon device (or when the check is forced). Same rule as :func:`check`."""
    device_marker = device_marker or os.environ.get("RACCOON_DEVICE_MARKER") or DEFAULT_DEVICE_MARKER
    if force is None:
        force = os.environ.get("RACCOON_CONTRACT_CHECK") == "force"
    return bool(force) or Path(device_marker).is_dir()


def aggregate(
    *,
    bundle_file: Optional[str] = None,
    directory: Optional[str] = None,
    device_marker: Optional[str] = None,
    force: Optional[bool] = None,
) -> dict[str, Any]:
    """What ``GET /contract`` of the Pi server returns.

    Off a Raccoon device (laptop, simulation, tests) there is nothing to
    enforce: the result is ``ok`` with ``on_device`` false.
    """
    bundle_file = bundle_file or os.environ.get("RACCOON_BUNDLE_FILE") or DEFAULT_BUNDLE_FILE
    if not on_device(device_marker, force):
        return {
            "on_device": False,
            "bundle": {"file": bundle_file, "error": None},
            "expected_hash": None,
            "dev_mode": False,
            "hashes_differ": False,
            "dev_components": [],
            "components": [],
            "blocking": [],
            "ok": True,
        }
    bundle: dict[str, Any] = {"file": bundle_file, "error": None}
    expected: Optional[str] = None
    try:
        document = json.loads(Path(bundle_file).read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            raise ValueError("not an object")
        bundle["name"] = document.get("bundle")
        components = document.get("components")
        bundle["transport_version"] = components.get("raccoon-transport") if isinstance(components, dict) else None
        value = document.get("contract_hash")
        expected = value.strip() if isinstance(value, str) else None
    except FileNotFoundError:
        bundle["error"] = "file-missing"
    except (OSError, ValueError):
        bundle["error"] = "file-unreadable"
    if bundle["error"] is None and expected is None:
        bundle["error"] = "field-missing"
    elif bundle["error"] is None and not expected:
        bundle["error"] = "field-empty"

    statuses = read_statuses(directory)
    dev_mode = expected == "dev"
    blocking = [
        s["component"]
        for s in statuses
        if s.get("result") in REFUSING and (s.get("active") or s.get("persistent"))
    ]
    if bundle["error"] is not None:
        blocking.insert(0, "bundle")
    active_hashes = {s.get("own_hash") for s in statuses if s.get("active") and s.get("own_hash")}
    # Components whose own bundle entry is 'dev': their version is not compared.
    dev_components = [
        s["component"]
        for s in statuses
        if s.get("expected_component_version") == "dev" and (s.get("active") or s.get("persistent"))
    ]
    return {
        "on_device": True,
        "bundle": bundle,
        "expected_hash": expected if bundle["error"] is None else None,
        "dev_mode": dev_mode,
        "hashes_differ": dev_mode and len(active_hashes) > 1,
        "dev_components": dev_components,
        "components": statuses,
        "blocking": blocking,
        "ok": not blocking,
    }


def refusal_message(summary: dict[str, Any]) -> Optional[str]:
    """Text for rejecting a run, or None when the run may start."""
    if summary["ok"]:
        return None
    parts = []
    if summary["bundle"]["error"]:
        parts.append(f"bundle file problem ({summary['bundle']['error']})")
    for entry in summary["components"]:
        if entry["component"] in summary["blocking"]:
            parts.append(f"{entry['component']}: {entry.get('message', entry.get('result'))}")
    return "Transport contract check failed; refusing to start. " + " | ".join(parts)


__all__ = [
    "BUNDLE_ERROR", "ContractError", "CheckResult", "DEV_WARNING", "EXIT_CODE", "MISMATCH", "OK",
    "SKIPPED", "VERSION_MISMATCH", "aggregate", "check", "exit_if_refused", "read_statuses", "refusal_message",
    "startup_check", "status_dir", "write_status", "asdict",
]
