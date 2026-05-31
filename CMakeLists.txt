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

# ---------- LCM ----------
# Message code generation still uses the installed LCM CMake package for
# lcm-gen, lcm_wrap_types(), and lcm_coretypes declarations. We no longer
# vendor or build libLCM from source in this tree.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLib2 REQUIRED IMPORTED_TARGET glib-2.0)
add_library(GLib2::glib INTERFACE IMPORTED GLOBAL)
set_target_properties(GLib2::glib PROPERTIES
  INTERFACE_LINK_LIBRARIES PkgConfig::GLib2)
find_package(lcm CONFIG REQUIRED)
include("${lcm_DIR}/lcmUtilities.cmake")

if(CMAKE_CROSSCOMPILING)
  # When cross-compiling, lcm-gen must execute on the host even if the imported
  # LCM package came from the target sysroot.
  find_program(_host_lcm_gen lcm-gen REQUIRED
    PATHS /usr/bin /usr/local/bin "$ENV{HOME}/.local/bin"
    NO_CMAKE_FIND_ROOT_PATH)
  set_target_properties(lcm-gen PROPERTIES IMPORTED_LOCATION "${_host_lcm_gen}")
  message(STATUS "Cross-compile: using host lcm-gen at ${_host_lcm_gen}")
endif()

# ---------- Messages ----------
add_subdirectory(messages)

# ---------- C++ Transport Library ----------
add_subdirectory(cpp)

# ---------- Interop Tools ----------
add_subdirectory(cpp/tools)
