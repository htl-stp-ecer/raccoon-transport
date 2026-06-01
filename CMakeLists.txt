set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

cmake_minimum_required(VERSION 3.22)
project(raccoon_transport VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- Transport backend ----------
# raccoon::Transport is backed by `raccoon_ring` (cpp/src/raccoon_ring.c) —
# a single-producer multi-consumer SHM ring buffer that lives in this
# tree. We previously vendored iceoryx2 v0.9.999-dev for this; it proved
# unreliable on the deployed Pi (NodeBuilder::create failing with
# InternalError once the stm32-data-reader had opened ~100 publishers,
# subscribes hanging indefinitely on v0.9.1, sporadic data-delivery
# failures even in isolated single-process repros). raccoon_ring is a
# direct in-process drop-in: one /dev/shm file per channel, per-slot
# SeqLock, pure polling. No daemon, no service descriptors, no marked-
# for-destruction state machine to get stuck in. See
# cpp/src/raccoon_ring.c for the implementation rationale.

# ---------- Messages ----------
add_subdirectory(messages)

# ---------- C++ Transport Library ----------
add_subdirectory(cpp)

# ---------- Interop Tools ----------
add_subdirectory(cpp/tools)
