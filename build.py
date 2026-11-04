#!/usr/bin/env python3
"""build.py — build the raccoon-transport Python wheel and the contract assets in Docker.

Nothing is installed on the host and nothing is written outside ./builds/: the sources are
copied into the container, built there, and only the results plus builds/manifest.json come
out. CI runs exactly this script.

Usage:
    python3 build.py                  # local build, version 0.0
    python3 build.py --version 71     # release build (CI)

Result:
    builds/raccoon_transport-<version>-py3-none-any.whl
    builds/contract-hash.txt          the contract hash (raccoon-image reads it from the release)
    builds/contract.json              the contract in readable form
    builds/manifest.json

C++ and Dart are used as source through the submodule and are not built here; their tests run
in CI (build-and-test).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILDS = ROOT / "builds"
IMAGE = "raccoon-transport-build"
VERSION_PATTERN = re.compile(r"^[0-9]+(\.[0-9]+)*$")

# Top-level entries that are not copied into the container.
# build.py and deploy.py stay out: `python -m build` would import ./build.py instead of the package.
EXCLUDE = {".git", "build", "builds", "__pycache__", "build.py", "deploy.py"}


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(1)


def run(command: list[str], **kwargs) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, **kwargs)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# ── inside the container ─────────────────────────────────────────────────


def run_in_container(version: str) -> None:
    """The actual build. /src is the repository (read-only), /out is ./builds."""
    src, work, out = Path("/src"), Path("/tmp/work"), Path("/out")
    shutil.copytree(src, work, ignore=lambda directory, names: (
        [n for n in names if n in EXCLUDE] if Path(directory) == src else []
    ))
    os.chdir(work)

    # Fails when the generated constants are stale or the Python/Dart messages no longer match
    # the C++ wire layout. A wheel with a wrong contract must never be built.
    run(["python3", "scripts/contract.py", "--check"])

    pyproject = work / "python" / "pyproject.toml"
    text, count = re.subn(r'^version = ".*"', f'version = "{version}"', pyproject.read_text(), flags=re.MULTILINE)
    if count != 1:
        fail("could not find the version in python/pyproject.toml")
    pyproject.write_text(text)

    run(["python3", "-m", "build", "--wheel", "--no-isolation", "--outdir", str(out), "python"])

    # The release carries the contract hash so the image build can learn the expected hash of a
    # transport version without building it.
    hash_text = subprocess.run(
        ["python3", "scripts/contract.py", "--print"], capture_output=True, text=True, check=True
    ).stdout
    (out / "contract-hash.txt").write_text(hash_text)
    shutil.copy(work / "contract" / "contract.json", out / "contract.json")


# ── on the host ──────────────────────────────────────────────────────────


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the raccoon-transport wheel in Docker.")
    parser.add_argument("--version", default="0.0", help="version stamped into the wheel (default: 0.0 = local build)")
    parser.add_argument("--image", help="use this Docker image instead of building ./Dockerfile")
    parser.add_argument("--in-container", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if not VERSION_PATTERN.match(args.version):
        fail(f"--version must be numbers separated by dots (got {args.version!r})")

    if args.in_container:
        run_in_container(args.version)
        return

    if not shutil.which("docker"):
        fail("docker not found. The build runs in a container so the host stays clean.")

    image = args.image
    if not image:
        image = IMAGE
        print(f"▶ Building Docker image {image}")
        subprocess.run(["docker", "build", "-t", image, "-f", str(ROOT / "Dockerfile"), str(ROOT)], check=True)

    BUILDS.mkdir(exist_ok=True)
    for stale in [*BUILDS.glob("raccoon_transport-*.whl"), BUILDS / "contract-hash.txt",
                  BUILDS / "contract.json", BUILDS / "manifest.json"]:
        stale.unlink(missing_ok=True)

    print(f"▶ Building raccoon-transport {args.version}")
    run([
        "docker", "run", "--rm",
        "--user", f"{os.getuid()}:{os.getgid()}",
        "-e", "HOME=/tmp",
        "-v", f"{ROOT}:/src:ro",
        "-v", f"{BUILDS}:/out",
        image,
        "python3", "/src/build.py", "--in-container", "--version", args.version,
    ])

    wheels = sorted(BUILDS.glob("raccoon_transport-*.whl"))
    if len(wheels) != 1:
        fail(f"expected exactly one wheel in {BUILDS}, found {len(wheels)}")
    files = [
        {"name": wheels[0].name, "kind": "wheel"},
        {"name": "contract-hash.txt", "kind": "contract-hash"},
        {"name": "contract.json", "kind": "contract"},
    ]
    for entry in files:
        entry["sha256"] = sha256(BUILDS / entry["name"])
    manifest = {
        "name": "raccoon-transport",
        "version": args.version,
        "contract_hash": (BUILDS / "contract-hash.txt").read_text().strip(),
        "files": files,
    }
    (BUILDS / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"✓ {wheels[0].relative_to(ROOT)}  (contract {manifest['contract_hash']})")


if __name__ == "__main__":
    main()
