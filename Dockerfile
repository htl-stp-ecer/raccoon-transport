# Build environment for raccoon-transport: the Python wheel and the contract assets.
# build.py builds this image and runs the build inside it, so nothing has to be installed on
# the host. C++ and Dart are used as source (submodule) and are tested in CI, not built here.
FROM python:3.13-slim

RUN pip install --no-cache-dir build "setuptools>=68.0" wheel
