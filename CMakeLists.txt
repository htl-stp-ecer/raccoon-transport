set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

cmake_minimum_required(VERSION 3.16)
project(raccoon_transport VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# ---------- LCM ----------
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

# ---------- Messages ----------
add_subdirectory(messages)

# ---------- C++ Transport Library ----------
add_subdirectory(cpp)

# ---------- Interop Tools ----------
add_subdirectory(cpp/tools)
