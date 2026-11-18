# Tele-operation: the state machine (M4.1) and the teleop core (M4.2). Plain C++, no Qt, so every
# safety rule is tested without a window, a GPU or a network.
add_library(msc_teleop STATIC
    src/state_machine.cpp
    src/teleop_core.cpp)
add_library(msc::teleop ALIAS msc_teleop)

target_include_directories(msc_teleop PUBLIC include)
target_link_libraries(msc_teleop PUBLIC msc::common msc::comms)
msc_set_warnings(msc_teleop)

if(BUILD_TESTING)
    add_executable(msc_teleop_tests
        tests/state_machine_test.cpp
        tests/teleop_arm_test.cpp
        tests/teleop_core_test.cpp)
    # msc::arm only for the test against the real arm controller.
    target_link_libraries(msc_teleop_tests PRIVATE msc::teleop msc::arm GTest::gtest_main)
    msc_set_warnings(msc_teleop_tests)
    gtest_discover_tests(msc_teleop_tests)
endif()
