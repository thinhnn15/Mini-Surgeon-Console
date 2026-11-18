# Video path pieces that need no Qt: the frame type, the test pattern, frame sources.
# Kept out of console_app so they build and test on any machine, including CI without a GPU.
find_package(Threads REQUIRED)

add_library(msc_video STATIC
    src/test_pattern.cpp
    src/synthetic_source.cpp
    src/presentation_tracker.cpp
    src/pixel_convert.cpp)
add_library(msc::video ALIAS msc_video)

target_include_directories(msc_video PUBLIC include)
target_link_libraries(msc_video PUBLIC msc::common Threads::Threads)
msc_set_warnings(msc_video)
if(WIN32)
    target_link_libraries(msc_video PRIVATE winmm)  # timeBeginPeriod, see synthetic_source.cpp
endif()

if(BUILD_TESTING)
    add_executable(msc_video_tests
        tests/test_pattern_test.cpp
        tests/synthetic_source_test.cpp
        tests/letterbox_test.cpp
        tests/presentation_tracker_test.cpp
        tests/pixel_convert_test.cpp
        tests/video_signal_test.cpp)
    target_link_libraries(msc_video_tests PRIVATE msc::video GTest::gtest_main)
    msc_set_warnings(msc_video_tests)
    gtest_discover_tests(msc_video_tests)
endif()
