cmake_minimum_required(VERSION 3.16)

add_library(raccoon_transport STATIC
    src/Transport.cpp
    src/ReliablePublisher.cpp
    src/ReliableSubscriber.cpp
    src/RetainStore.cpp
)

add_library(raccoon::transport ALIAS raccoon_transport)

target_include_directories(raccoon_transport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(raccoon_transport PUBLIC
    raccoon-messages-cpp
        ${LCM_NAMESPACE}lcm-static
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(raccoon_transport PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (BUILD_TESTING)
    add_executable(raccoon_transport_unit_tests
        tests/transport_unit_tests.cpp
    )

    target_link_libraries(raccoon_transport_unit_tests PRIVATE
        raccoon_transport
    )

    target_include_directories(raccoon_transport_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(raccoon_transport_unit_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME raccoon_transport_unit_tests COMMAND raccoon_transport_unit_tests)
endif()
