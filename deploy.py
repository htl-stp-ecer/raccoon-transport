#!/usr/bin/env python3
"""deploy.py — install a local raccoon-transport wheel on a Pi (for development).

Copies the wheel from this machine to the Pi and installs it there. Releases are installed by
`raccoon update` (from PyPI), not by this script. Standard library only.

Usage:
    python3 deploy.py --host 192.168.4.1                          # the wheel from the last build.py run
    python3 deploy.py --host 192.168.4.1 --artifact my.whl        # a specific wheel

raccoon-transport is set to "dev" in the Pi's /etc/raccoon-bundle.json: a deployed wheel is a
custom build. Note what that does and does not do: the transport version in that file is never
compared, so "dev" is only a marker. What the components check is the contract HASH. If the
wheel's contract differs from the bundle's, components built for the bundle's contract refuse to
start runs until they are rebuilt too; this script prints a warning in that case.
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path

COMPONENT = "raccoon-transport"
BUNDLE_FILE = "/etc/raccoon-bundle.json"
REMOTE_TMP = "/tmp/raccoon-transport-install"

# Runs on the Pi as root: set this component's version to "dev" in the bundle file and
# leave everything else in it as it is.
MARK_DEV = r"""
import json, os, tempfile
path = "%(path)s"
try:
    with open(path) as handle:
        document = json.load(handle)
    if not isinstance(document, dict):
        document = {}
except (OSError, ValueError):
    document = {}
document.setdefault("bundle", "dev")
components = document.get("components")
if not isinstance(components, dict):
    components = {}
components["%(component)s"] = "dev"
document["components"] = components
fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".raccoon-bundle.")
with os.fdopen(fd, "w") as handle:
    json.dump(document, handle, indent=2)
    handle.write("\n")
os.chmod(tmp, 0o644)
os.replace(tmp, path)
os.makedirs("/run/raccoon", exist_ok=True)
os.chmod("/run/raccoon", 0o1777)
"""

# Prints the contract_hash of the bundle file on the Pi (nothing if there is none).
READ_HASH = r"""
import json
try:
    value = json.load(open("%(path)s")).get("contract_hash")
except (OSError, ValueError, AttributeError):
    value = None
print(value if isinstance(value, str) else "")
"""


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(1)


class Pi:
    def __init__(self, host: str, user: str, ssh_params: str) -> None:
        self.target = f"{user}@{host}"
        self.params = shlex.split(ssh_params)

    def _command(self, command: str) -> list[str]:
        return ["ssh", "-o", "ConnectTimeout=5", *self.params, self.target, command]

    def run(self, command: str, *, check: bool = True, stdin: str | None = None) -> int:
        result = subprocess.run(self._command(command), input=stdin, text=True)
        if check and result.returncode != 0:
            fail(f"command failed on the Pi: {command}")
        return result.returncode

    def output(self, command: str, *, stdin: str | None = None) -> str:
        result = subprocess.run(self._command(command), input=stdin, text=True, capture_output=True)
        return result.stdout.strip() if result.returncode == 0 else ""

    def copy(self, local: Path, remote_dir: str) -> None:
        subprocess.run(["scp", *self.params, str(local), f"{self.target}:{remote_dir}/"], check=True)


def find_wheel(artifact: str | None) -> tuple[Path, str | None]:
    """The wheel to install and, if known, the contract hash it was built with."""
    contract_hash = None
    if artifact:
        path = Path(artifact)
        hash_file = path.parent / "contract-hash.txt"
        if hash_file.is_file():
            contract_hash = hash_file.read_text().strip() or None
    else:
        manifest = Path(__file__).resolve().parent / "builds" / "manifest.json"
        if not manifest.is_file():
            fail("no builds/manifest.json. Run build.py first, or pass --artifact <wheel>.")
        document = json.loads(manifest.read_text())
        wheels = [f["name"] for f in document["files"] if f.get("kind") == "wheel"]
        if len(wheels) != 1:
            fail(f"expected one wheel in {manifest}, found {len(wheels)}")
        path = manifest.parent / wheels[0]
        contract_hash = document.get("contract_hash")
    if not path.is_file() or not (path.name.startswith("raccoon_transport-") and path.suffix == ".whl"):
        fail(f"not a raccoon_transport wheel: {path}")
    return path, contract_hash


def main() -> None:
    parser = argparse.ArgumentParser(description="Install a raccoon-transport wheel on a Pi.")
    parser.add_argument("--host", required=True, help="Pi address")
    parser.add_argument("--user", default="pi", help="SSH user (default: pi)")
    parser.add_argument("--artifact", help="wheel to install (default: the one in builds/manifest.json)")
    parser.add_argument("--ssh-params", default="", help='extra ssh/scp options, e.g. "-o StrictHostKeyChecking=no"')
    args = parser.parse_args()

    wheel, wheel_hash = find_wheel(args.artifact)
    pi = Pi(args.host, args.user, args.ssh_params)
    print(f"Deploying {wheel.name} to {pi.target}")

    print("Testing SSH connection...")
    if pi.run("true", check=False) != 0:
        fail(f"cannot connect to {pi.target} (see the ssh message above). "
             f"Host key changed (e.g. after flashing a new image)?  ssh-keygen -R {args.host}")

    print("Uploading wheel...")
    pi.run(f"rm -rf {REMOTE_TMP} && mkdir -p {REMOTE_TMP}")
    pi.copy(wheel, REMOTE_TMP)

    print("Installing...")
    # The wheel is pure Python without dependencies. A user-level install would shadow it.
    pi.run("python3 -m pip uninstall raccoon-transport -y --break-system-packages 2>/dev/null || true", check=False)
    pi.run(
        "sudo pip3 install --break-system-packages --force-reinstall --no-deps "
        f"{shlex.quote(f'{REMOTE_TMP}/{wheel.name}')}"
    )

    # The bundle file must be right before the service starts: it checks itself against it.
    print(f'Marking {COMPONENT} as "dev" in {BUNDLE_FILE}...')
    pi.run("sudo python3 -", stdin=MARK_DEV % {"path": BUNDLE_FILE, "component": COMPONENT})

    bundle_hash = pi.output("python3 -", stdin=READ_HASH % {"path": BUNDLE_FILE})
    if wheel_hash and bundle_hash and wheel_hash != bundle_hash and bundle_hash != "dev":
        print()
        print(f"WARNING: this wheel's contract is {wheel_hash}, the bundle on the Pi expects {bundle_hash}.")
        print("  Components built for the bundle's contract (raccoon-lib, stm32-data-reader, botui) will")
        print("  refuse to start runs until they are rebuilt and deployed with this contract.")
        print('  Marking the transport as "dev" does not change that: only contract_hash = "dev" does.')

    # The raccoon server imports the transport at start.
    print("Restarting the raccoon service...")
    pi.run("sudo systemctl restart raccoon.service")

    print()
    pi.run("systemctl is-active raccoon.service; python3 -c 'import raccoon_transport as t; print(t.transport_version(), t.contract_hash())'",
           check=False)
    print(f"Deployment to {args.host} completed.")


if __name__ == "__main__":
    main()
