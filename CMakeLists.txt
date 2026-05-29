set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

cmake_minimum_required(VERSION 3.22)
project(raccoon_transport VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# ---------- iceoryx2 ----------
# Shared-memory pub/sub. Replaces the LCM UDP-multicast transport that
# kept dropping packets on the production Pi (see RESULTS.md in the
# benchmark history). The Rust core is built via cargo; the C++ binding
# layer is added via the iceoryx2 CMake workspace. Requires rustc/cargo
# and libclang on PATH (added to the manylinux build image — see
# .github/docker/Dockerfile).
#
# Allow an external install to short-circuit the build (e.g. `pip install`
# environments that vendor a pre-built iceoryx2). Setting
# RACCOON_USE_SYSTEM_ICEORYX2=ON tells us to find_package() and skip the
# FetchContent build.
option(RACCOON_USE_SYSTEM_ICEORYX2
    "Use an externally installed iceoryx2 (find_package) instead of FetchContent" OFF)
# Pin to the commit we built and verified on the Pi (benchmark + smoke test).
# Bump deliberately, not opportunistically.
set(IOX2_GIT_TAG "a1bf22f688a3a5da5704ae52e006164c4e65a984"
    CACHE STRING "iceoryx2 git revision to vendor")

if(RACCOON_USE_SYSTEM_ICEORYX2)
    find_package(iceoryx2-cxx 0.9.999 REQUIRED)
else()
    # Disable example builds: they failed our Pi build with "No space left on
    # device" on a stuffed SD card and they bring nothing we need. Disable
    # tests for the same reason.
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING_TMP ${BUILD_TESTING})
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        iceoryx2
        GIT_REPOSITORY https://github.com/eclipse-iceoryx/iceoryx2.git
        GIT_TAG ${IOX2_GIT_TAG}
        # GIT_SHALLOW omitted on purpose: we pin to a SHA, and shallow clones
        # of arbitrary SHAs are not supported by the dumb HTTP transport.
    )
    FetchContent_MakeAvailable(iceoryx2)
    set(BUILD_TESTING ${BUILD_TESTING_TMP} CACHE BOOL "" FORCE)
endif()

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
