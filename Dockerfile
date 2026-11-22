# Builder for the raccoon-transport wheels and the contract assets. Used by build.py, which picks
# the stage with `docker build --target`. Like the raccoon-lib builder:
#
#   native  the manylinux_2_28 image of the machine it is built on (an aarch64 machine, e.g. the CI's
#           ARM runner). The wheel is compiled there and checked with auditwheel, so it carries a
#           manylinux tag: it is the wheel that goes to GitHub and to PyPI. Everything runs natively.
#   cross   x86_64 Debian image with the aarch64 cross compiler (a PC). Nothing is emulated while
#           compiling. The wheel has the tag linux_aarch64 and is only for deploy.py on a Pi; it is
#           tried out under qemu-user (an import and a publish/subscribe round trip).
ARG MANYLINUX=quay.io/pypa/manylinux_2_28_x86_64

FROM ${MANYLINUX} AS native
ENV PATH=/opt/python/cp313-cp313/bin:$PATH \
    PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
RUN pip install --no-cache-dir build "setuptools>=68.0" wheel "pybind11>=2.11"
WORKDIR /src


FROM debian:13-slim AS base

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates python3 python3-dev python3-venv python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Python build tools (host).
ENV VIRTUAL_ENV=/opt/venv
RUN python3 -m venv $VIRTUAL_ENV
ENV PATH=/opt/venv/bin:$PATH
RUN pip install --no-cache-dir build "setuptools>=68.0" wheel "pybind11>=2.11"

ENV PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
WORKDIR /src


FROM base AS cross
RUN dpkg --add-architecture arm64

RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-user \
    && rm -rf /var/lib/apt/lists/*

# aarch64 Python 3.13: the headers to compile against, and a runtime for qemu-user.
# python3.13-minimal:arm64 cannot be installed (its postinst would run an ARM binary), so the
# packages are only downloaded and unpacked into /opt/arm64-root.
RUN apt-get update \
    && apt-get install -y --no-install-recommends --download-only \
         libpython3.13-dev:arm64 python3.13-minimal:arm64 libpython3.13-stdlib:arm64 \
         libstdc++6:arm64 libgcc-s1:arm64 libc6:arm64 \
    && mkdir -p /opt/arm64-root \
    && for deb in /var/cache/apt/archives/*_arm64.deb; do dpkg-deb --extract "$deb" /opt/arm64-root; done \
    && ln -s usr/lib /opt/arm64-root/lib \
    && mkdir -p /usr/include/aarch64-linux-gnu \
    && cp -r /opt/arm64-root/usr/include/aarch64-linux-gnu/python3.13 /usr/include/aarch64-linux-gnu/ \
    && rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb
CMD ["/bin/bash"]
