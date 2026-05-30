set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

cmake_minimum_required(VERSION 3.22)
project(raccoon_transport VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

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

# ---------- LCM ----------
# When cross-compiling, lcm-gen must execute on the HOST (x86_64), but the LCM
# C library must be compiled for the TARGET (aarch64).
# Strategy: detect cross-compilation and use the system-installed host lcm-gen
# for code generation, while still building the LCM library from source for the
# target architecture via FetchContent.
set(LCM_ENABLE_JAVA OFF CACHE BOOL "" FORCE)
set(LCM_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(LCM_ENABLE_DOCS OFF CACHE BOOL "" FORCE)
set(LCM_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    lcm
    GIT_REPOSITORY https://github.com/lcm-proj/lcm.git
    GIT_TAG v1.5.0
)
FetchContent_MakeAvailable(lcm)

# lcm_wrap_types is defined in lcmUtilities.cmake
include(${lcm_SOURCE_DIR}/lcm-cmake/lcmUtilities.cmake)

if(CMAKE_CROSSCOMPILING)
  # When cross-compiling, lcm-gen was not built (lcmgen/ subdir is skipped via
  # a patch applied by build-cross.sh). Provide it as an IMPORTED EXECUTABLE
  # pointing to the host-installed lcm-gen so that lcm_wrap_types() custom
  # commands (which reference the "lcm-gen" CMake target) work correctly.
  find_program(_host_lcm_gen lcm-gen REQUIRED
    PATHS /usr/bin /usr/local/bin
    NO_CMAKE_FIND_ROOT_PATH)
  add_executable(lcm-gen IMPORTED GLOBAL)
  set_target_properties(lcm-gen PROPERTIES IMPORTED_LOCATION "${_host_lcm_gen}")
  message(STATUS "Cross-compile: using host lcm-gen at ${_host_lcm_gen}")
endif()

# ---------- Messages ----------
add_subdirectory(messages)

# ---------- C++ Transport Library ----------
add_subdirectory(cpp)

# ---------- Interop Tools ----------
add_subdirectory(cpp/tools)
