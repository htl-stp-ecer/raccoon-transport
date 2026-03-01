cmake_minimum_required(VERSION 3.10)
project(raccoon_messages)

# Existing exlcm message types
file(GLOB exlcm_definitions CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/types/*.lcm")

# Protocol message types (raccoon package)
file(GLOB protocol_definitions CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/protocol/*.lcm")

set(all_definitions ${exlcm_definitions} ${protocol_definitions})

# Generate C/C++ code from all .lcm files
lcm_wrap_types(
    C_HEADERS   c_headers
    C_SOURCES   c_sources
    CPP_HEADERS cpp_headers
    ${all_definitions}
)

# C library
lcm_add_library(raccoon-messages-c C ${c_sources})
target_include_directories(raccoon-messages-c PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
target_link_libraries(raccoon-messages-c PUBLIC ${LCM_NAMESPACE}lcm-static)

# C++ headers-only interface
lcm_add_library(raccoon-messages-cpp CPP ${cpp_headers})
target_include_directories(raccoon-messages-cpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
target_link_libraries(raccoon-messages-cpp INTERFACE ${LCM_NAMESPACE}lcm-static)
