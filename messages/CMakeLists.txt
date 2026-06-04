cmake_minimum_required(VERSION 3.22)
project(raccoon_messages LANGUAGES CXX)

add_library(raccoon_messages STATIC
    protocol/ack_t.cpp
    protocol/envelope_t.cpp
    protocol/retain_request_t.cpp
    types/cam_blob_t.cpp
    types/cam_config_t.cpp
    types/cam_detections_t.cpp
    types/cam_frame_t.cpp
    types/cam_stream_ctl_t.cpp
    types/kinematics_config_t.cpp
    types/orientation_matrix_t.cpp
    types/quaternion_t.cpp
    types/scalar_f_t.cpp
    types/scalar_i32_t.cpp
    types/scalar_i8_t.cpp
    types/screen_render_answer_t.cpp
    types/screen_render_t.cpp
    types/string_t.cpp
    types/vector3f_t.cpp
    types/yolo_box_t.cpp
    types/yolo_frame_t.cpp
)

target_include_directories(raccoon_messages PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    
    
)

add_library(raccoon::messages ALIAS raccoon_messages)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(raccoon_messages PRIVATE -Wall -Wextra -Wpedantic)
endif()
