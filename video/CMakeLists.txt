# Video path pieces that need no Qt: the frame type, the test pattern, frame sources.
# Kept out of console_app so they build and test on any machine, including CI without a GPU.
add_library(msc_video STATIC
    src/test_pattern.cpp)
add_library(msc::video ALIAS msc_video)

target_include_directories(msc_video PUBLIC include)
target_link_libraries(msc_video PUBLIC msc::common)
msc_set_warnings(msc_video)

if(BUILD_TESTING)
    add_executable(msc_video_tests
        tests/test_pattern_test.cpp)
    target_link_libraries(msc_video_tests PRIVATE msc::video GTest::gtest_main)
    msc_set_warnings(msc_video_tests)
    gtest_discover_tests(msc_video_tests)
endif()
