cmake_minimum_required(VERSION 3.22)

# raccoon_transport is now backed by the in-tree raccoon_ring SHM library
# (src/raccoon_ring.c) — see the parent CMakeLists.txt comment for the
# May 2026 rationale on removing the iceoryx2 dependency.

add_library(raccoon_transport STATIC
    src/Transport.cpp
    src/raccoon_ring.c
)

add_library(raccoon::transport ALIAS raccoon_transport)

target_include_directories(raccoon_transport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(raccoon_transport PUBLIC
    raccoon-messages-cpp
)
# raccoon_ring uses POSIX shm + pthreads via stdatomic; on glibc both are
# in libc. Link explicitly anyway in case some downstream uses a libc
# variant that breaks them out.
target_link_libraries(raccoon_transport PUBLIC pthread)

# raccoon_ring.c is C11 (needs _Atomic + stdatomic.h).
set_source_files_properties(src/raccoon_ring.c
    PROPERTIES LANGUAGE C C_STANDARD 11)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(raccoon_transport PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (BUILD_TESTING)
    add_executable(raccoon_transport_producer_restart_tests
        tests/test_producer_restart.cpp)
    target_link_libraries(raccoon_transport_producer_restart_tests PRIVATE
        raccoon_transport)
    target_include_directories(raccoon_transport_producer_restart_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME raccoon_transport_producer_restart_tests
        COMMAND raccoon_transport_producer_restart_tests)

    add_executable(raccoon_transport_unit_tests tests/transport_unit_tests.cpp)
    target_link_libraries(raccoon_transport_unit_tests PRIVATE raccoon_transport)
    target_include_directories(raccoon_transport_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME raccoon_transport_unit_tests COMMAND raccoon_transport_unit_tests)
endif()
