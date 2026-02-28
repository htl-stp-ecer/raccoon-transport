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
    ${LCM_NAMESPACE}lcm
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(raccoon_transport PRIVATE -Wall -Wextra -Wpedantic)
endif()
