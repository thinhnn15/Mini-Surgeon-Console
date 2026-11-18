# The arm link: wire protocol (and, from M3.2, UDP). Plain C++, no Qt: arm_sim uses it too.
add_library(msc_comms STATIC
    src/protocol.cpp
    src/sequence_filter.cpp)
add_library(msc::comms ALIAS msc_comms)

target_include_directories(msc_comms PUBLIC include)
target_link_libraries(msc_comms PUBLIC msc::common)
msc_set_warnings(msc_comms)

if(BUILD_TESTING)
    add_executable(msc_comms_tests
        tests/protocol_test.cpp
        tests/sequence_filter_test.cpp)
    target_link_libraries(msc_comms_tests PRIVATE msc::comms GTest::gtest_main)
    msc_set_warnings(msc_comms_tests)
    gtest_discover_tests(msc_comms_tests)
endif()
