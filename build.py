#!/usr/bin/env python3
"""build.py — build the raccoon-transport Python wheel and the contract assets in Docker.

Nothing is installed on the host and nothing is written outside ./builds/: the sources are
copied into the container, built there, and only the results plus builds/manifest.json come
out. CI runs exactly this script.

Two modes, picked by the CPU of the machine it runs on (or --mode):

  cross   (x86_64 PC)   the native module is cross compiled with the aarch64 compiler at native
                        speed, nothing is emulated. Only the finished wheel is tried out under
                        qemu-user (no binfmt, no --privileged).
  native  (aarch64)     compiled with the normal compiler of an arm64 image, everything runs
                        natively. This is what the CI does on an ARM runner.

Usage:
    python3 build.py                  # local build, version 0.0
    python3 build.py --version 71     # release build (CI)
    python3 build.py --mode native    # force a mode (native on x86 makes an x86_64 wheel, for testing the script)

Result:
    builds/raccoon_transport-<version>-py3-none-any.whl                the pure wheel: channel names,
                                                                       message classes, contract check
    builds/raccoon_transport-<version>-cp313-cp313-linux_aarch64.whl   the native wheel for the Pi
                                                                       (compiled from ./cpp and ./messages)
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
import platform
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILDS = ROOT / "builds"
IMAGE = "raccoon-transport-build"  # + "-cross" / "-native"
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


ARM_ROOT = "/opt/arm64-root"
EXT_SUFFIX = ".cpython-313-aarch64-linux-gnu.so"


def build_native_wheel(work: Path, out: Path, mode: str) -> None:
    """The wheel with the compiled transport. cross: compiled for the Pi with the aarch64 compiler
    and tried out under qemu-user; native: compiled for the machine the container runs on."""
    native_out = Path("/tmp/native-out")
    native_out.mkdir()
    env = dict(os.environ)
    if mode == "cross":
        prefix = "aarch64-linux-gnu-"
        env.update(
            CC=f"{prefix}gcc", CXX=f"{prefix}g++",
            LDSHARED=f"{prefix}gcc -shared", LDCXXSHARED=f"{prefix}g++ -shared",
            # the headers of the target Python first: its pyconfig.h, not the one of the host's
            CFLAGS="-I/usr/include/aarch64-linux-gnu/python3.13",
            # the wheel is tagged and named for the Pi, not for the PC it is built on
            _PYTHON_HOST_PLATFORM="linux-aarch64", SETUPTOOLS_EXT_SUFFIX=EXT_SUFFIX,
        )
    run(["python3", "-m", "build", "--wheel", "--no-isolation", "--outdir", str(native_out), "python"], env=env)
    wheels = sorted(native_out.glob("*.whl"))
    if len(wheels) != 1:
        fail(f"expected one native wheel, found {len(wheels)}")
    smoke_test(wheels[0], mode)
    shutil.copy(wheels[0], out / wheels[0].name)


SMOKE_TEST = """
import raccoon_transport as rt, time
from raccoon_transport import channels
from raccoon_transport.types.raccoon import scalar_i32_t
assert rt.NATIVE, "no native module"
t = rt.Transport()
got = []
channel = channels.hardware.motor.cmd.power(0)
t.subscribe(channel, lambda ch, d: got.append(d))
t.publish(channel, scalar_i32_t(timestamp=0, value=7))
for _ in range(500):
    if got: break
    time.sleep(0.01)
assert got and scalar_i32_t.decode(got[0]).value == 7, "no frame came back"
t.shutdown()
print("native transport ok")
"""


def smoke_test(wheel: Path, mode: str) -> None:
    """Install the wheel and send a frame through it (cross: with the aarch64 Python under qemu-user)."""
    site = Path("/tmp/smoke-site")
    shutil.rmtree(site, ignore_errors=True)
    platform_args = (["--python-version", "3.13", "--implementation", "cp", "--platform", "linux_aarch64",
                      "--only-binary=:all:"] if mode == "cross" else [])
    run(["pip", "install", "--quiet", "--target", str(site), "--no-deps", *platform_args, str(wheel)])
    interpreter = (["qemu-aarch64", "-L", ARM_ROOT, f"{ARM_ROOT}/usr/bin/python3.13"] if mode == "cross"
                   else ["python3"])
    env = {"PYTHONPATH": str(site), "PYTHONDONTWRITEBYTECODE": "1", "HOME": "/tmp", "PATH": os.environ["PATH"],
           "RACCOON_TRANSPORT_NO_EXIT_HOOK": "1"}
    run([*interpreter, "-c", SMOKE_TEST], env=env)


def run_in_container(version: str, mode: str) -> None:
    """The actual build. /src is the repository (read-only), /out is ./builds."""
    src, work, out = Path("/src"), Path("/tmp/work"), Path("/out")
    shutil.copytree(src, work, ignore=lambda directory, names: (
        [n for n in names if n in EXCLUDE] if Path(directory) == src else []
    ))
    os.chdir(work)

    # Fails when the generated constants are stale or the Python/Dart messages no longer match
    # the C++ wire layout. A wheel with a wrong contract must never be built.
    run(["python3", "scripts/contract.py", "--check"])
    run(["python3", "scripts/channels.py", "--check"])

    pyproject = work / "python" / "pyproject.toml"
    text, count = re.subn(r'^version = ".*"', f'version = "{version}"', pyproject.read_text(), flags=re.MULTILINE)
    if count != 1:
        fail("could not find the version in python/pyproject.toml")
    pyproject.write_text(text)

    # The pure wheel has no compiled part; it is what a laptop installs.
    run(["python3", "-m", "build", "--wheel", "--no-isolation", "--outdir", str(out), "python"],
        env={**os.environ, "RACCOON_TRANSPORT_PURE": "1"})
    build_native_wheel(work, out, mode)

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
    parser.add_argument("--mode", choices=["auto", "cross", "native"], default="auto",
                        help="auto: cross on an x86 machine, native on an aarch64 machine")
    parser.add_argument("--image", help="use this Docker image instead of building ./Dockerfile")
    parser.add_argument("--in-container", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if not VERSION_PATTERN.match(args.version):
        fail(f"--version must be numbers separated by dots (got {args.version!r})")

    if args.in_container:
        run_in_container(args.version, args.mode)
        return

    if not shutil.which("docker"):
        fail("docker not found. The build runs in a container so the host stays clean.")

    mode = args.mode
    if mode == "auto":
        mode = "native" if platform.machine() in ("aarch64", "arm64") else "cross"
    image = args.image
    if not image:
        image = f"{IMAGE}-{mode}"
        print(f"▶ Building Docker image {image} ({mode})")
        subprocess.run(["docker", "build", "--target", mode, "-t", image, "-f", str(ROOT / "Dockerfile"), str(ROOT)],
                       check=True)

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
        "--mode", mode,
    ])

    wheels = sorted(BUILDS.glob("raccoon_transport-*.whl"))
    if len(wheels) != 2:
        fail(f"expected the pure and the native wheel in {BUILDS}, found {len(wheels)}")
    files = [
        *({"name": w.name, "kind": "wheel" if w.name.endswith("-any.whl") else "native-wheel"} for w in wheels),
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
    for wheel in wheels:
        print(f"✓ {wheel.relative_to(ROOT)}  (contract {manifest['contract_hash']})")


if __name__ == "__main__":
    main()
