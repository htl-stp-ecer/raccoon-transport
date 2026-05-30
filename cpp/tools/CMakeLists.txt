cmake_minimum_required(VERSION 3.16)

add_executable(interop_publish interop_publish.cpp)
target_link_libraries(interop_publish PRIVATE raccoon::transport)

add_executable(interop_subscribe interop_subscribe.cpp)
target_link_libraries(interop_subscribe PRIVATE raccoon::transport)

# iox2_janitor.cpp is intentionally NOT built any more — raccoon_transport
# no longer uses iceoryx2 (see the parent CMakeLists.txt comment), so
# there is nothing for the janitor to sweep. The .cpp is kept on disk
# for reference until the next cleanup pass; the matching systemd unit
# in stm32-data-reader/systemd/ should also be removed at the same time.
