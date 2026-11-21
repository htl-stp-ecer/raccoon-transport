set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

cmake_minimum_required(VERSION 3.22)
project(raccoon_transport VERSION 0.1.0 LANGUAGES C CXX)

# Human-readable transport release, shown in version/contract messages. The
# contract hash, not this number, decides whether components fit together.
set(RACCOON_TRANSPORT_VERSION "dev" CACHE STRING "Transport release shown in messages")

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- Transport backend ----------
# raccoon::Transport is backed by `raccoon_ring` (cpp/src/raccoon_ring.c): a
# shared-memory ring buffer with one writer and any number of readers, one
# /dev/shm file per channel, a seqlock per slot and futex wake-ups. No daemon and
# no service descriptors. See cpp/include/raccoon/raccoon_ring.h for the design.

# ---------- Messages ----------
add_subdirectory(messages)

# ---------- C++ Transport Library ----------
add_subdirectory(cpp)

# ---------- Interop Tools ----------
add_subdirectory(cpp/tools)
