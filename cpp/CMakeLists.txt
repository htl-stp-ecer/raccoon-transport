cmake_minimum_required(VERSION 3.22)

# iceoryx2 is brought in by the parent CMakeLists.txt either via
# FetchContent (default) or via find_package() when
# RACCOON_USE_SYSTEM_ICEORYX2=ON. Either path exposes the same alias
# target name `iceoryx2-cxx::static-lib-cxx`.

add_library(raccoon_transport STATIC
    src/Transport.cpp
)

add_library(raccoon::transport ALIAS raccoon_transport)

target_include_directories(raccoon_transport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(raccoon_transport PUBLIC
    raccoon-messages-cpp
        iceoryx2-cxx::static-lib-cxx
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(raccoon_transport PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Unit tests are temporarily disabled: they deep-test the LCM-channel-based
# reliable/retain protocols that have not yet been ported to iceoryx2.
# Re-enable once a fresh test surface is written for the iceoryx2 backend.
if (BUILD_TESTING AND RACCOON_TRANSPORT_BUILD_LCM_TESTS)
    add_executable(raccoon_transport_unit_tests tests/transport_unit_tests.cpp)
    target_link_libraries(raccoon_transport_unit_tests PRIVATE raccoon_transport)
    target_include_directories(raccoon_transport_unit_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME raccoon_transport_unit_tests COMMAND raccoon_transport_unit_tests)
endif()
