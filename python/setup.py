"""Build of the native module `raccoon_transport._native`.

The module compiles the C++ core of this repository (../cpp, ../messages). With
RACCOON_TRANSPORT_PURE=1 nothing is compiled: the wheel then only holds the pure
Python parts (message classes, channel names, contract check), which is what a
laptop needs. The compiler flags come from the environment (CC, CXX, CFLAGS).
"""
import os
from pathlib import Path

from setuptools import Extension, setup

ROOT = Path(__file__).resolve().parent.parent

ext_modules = []
if os.environ.get("RACCOON_TRANSPORT_PURE") != "1":
    import pybind11

    here = Path(__file__).resolve().parent
    messages = sorted(os.path.relpath(p, here) for p in (ROOT / "messages").glob("*/*.cpp"))
    ext_modules.append(
        Extension(
            "raccoon_transport._native",
            sources=[
                "src/native.cpp",
                os.path.relpath(ROOT / "cpp" / "src" / "Transport.cpp", here),
                os.path.relpath(ROOT / "cpp" / "src" / "raccoon_ring.c", here),
                *messages,
            ],
            include_dirs=[
                pybind11.get_include(),
                str(ROOT / "cpp" / "include"),
                str(ROOT / "messages" / "include"),
            ],
            extra_compile_args=["-std=c++20", "-O2", "-fvisibility=hidden"],
            language="c++",
        )
    )

setup(ext_modules=ext_modules)
