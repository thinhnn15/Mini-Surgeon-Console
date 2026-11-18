# The arm link: wire protocol and UDP. Plain C++, no Qt: arm_sim uses it too.
add_library(msc_comms STATIC
    src/arm_link.cpp
    src/protocol.cpp
    src/sequence_filter.cpp
    src/session.cpp
    src/udp_socket.cpp)
add_library(msc::comms ALIAS msc_comms)

target_include_directories(msc_comms PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(msc_comms PUBLIC msc::common Threads::Threads)
msc_set_warnings(msc_comms)
if(WIN32)
    target_link_libraries(msc_comms PRIVATE ws2_32)  # Winsock
endif()

if(BUILD_TESTING)
    add_executable(msc_comms_tests
        tests/arm_link_test.cpp
        tests/heartbeat_monitor_test.cpp
        tests/protocol_test.cpp
        tests/sequence_filter_test.cpp
        tests/udp_socket_test.cpp)
    target_link_libraries(msc_comms_tests PRIVATE msc::comms GTest::gtest_main)
    msc_set_warnings(msc_comms_tests)
    gtest_discover_tests(msc_comms_tests)
endif()
