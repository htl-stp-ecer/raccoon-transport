cmake_minimum_required(VERSION 3.16)

add_executable(interop_publish interop_publish.cpp)
target_link_libraries(interop_publish PRIVATE raccoon::transport)

add_executable(interop_subscribe interop_subscribe.cpp)
target_link_libraries(interop_subscribe PRIVATE raccoon::transport)
