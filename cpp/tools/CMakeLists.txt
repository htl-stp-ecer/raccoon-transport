cmake_minimum_required(VERSION 3.16)

add_executable(interop_publish interop_publish.cpp)
target_link_libraries(interop_publish PRIVATE raccoon::transport)

add_executable(interop_subscribe interop_subscribe.cpp)
target_link_libraries(interop_subscribe PRIVATE raccoon::transport)

# iox2 dead-node janitor — long-lived systemd service that periodically
# calls Node::try_cleanup_dead_nodes(). iceoryx2 is daemon-less by
# design (no RouDi), so orphan node descriptors stay under
# /tmp/iceoryx2/ whenever a process dies without running its
# destructors. The janitor sweeps them out continuously so the next
# Node::create() in any raccoon process finds a clean state. Links
# only against iceoryx2-cxx — no raccoon::transport dependency —
# keeping the binary small and independent of the raccoon wheel.
add_executable(iox2_janitor iox2_janitor.cpp)
target_link_libraries(iox2_janitor PRIVATE iceoryx2-cxx::static-lib-cxx)
