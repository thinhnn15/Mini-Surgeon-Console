# The overlay's logic (M5): where the arm was at the time a video frame was captured, and
# whether that is known at all. Plain C++, no Qt, so REQ-HUD-001 is tested without a window.
add_library(msc_hud STATIC
    src/pose_history.cpp
    src/tool_camera.cpp)
add_library(msc::hud ALIAS msc_hud)

target_include_directories(msc_hud PUBLIC include)
# msc::comms for the joint types the arm reports.
target_link_libraries(msc_hud PUBLIC msc::common msc::comms)
msc_set_warnings(msc_hud)

if(BUILD_TESTING)
    add_executable(msc_hud_tests
        tests/pose_feed_test.cpp
        tests/pose_history_test.cpp
        tests/tool_camera_test.cpp)
    target_link_libraries(msc_hud_tests PRIVATE msc::hud GTest::gtest_main)
    msc_set_warnings(msc_hud_tests)
    gtest_discover_tests(msc_hud_tests)
endif()
