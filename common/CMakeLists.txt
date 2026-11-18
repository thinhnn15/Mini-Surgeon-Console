# Plain C++ shared by arm_sim and console_app. No Qt here, on purpose.
add_library(msc_common STATIC
    src/crc32.cpp
    src/fine_timer.cpp
    src/latency_stats.cpp)
add_library(msc::common ALIAS msc_common)

target_include_directories(msc_common PUBLIC include)
target_compile_features(msc_common PUBLIC cxx_std_20)
msc_set_warnings(msc_common)
if(WIN32)
    target_link_libraries(msc_common PRIVATE winmm)  # timeBeginPeriod, see fine_timer.hpp
endif()

if(BUILD_TESTING)
    add_executable(msc_common_tests
        tests/crc32_test.cpp
        tests/clock_test.cpp
        tests/latest_value_test.cpp
        tests/triple_buffer_test.cpp
        tests/latency_stats_test.cpp)
    target_link_libraries(msc_common_tests PRIVATE msc::common GTest::gtest_main)
    msc_set_warnings(msc_common_tests)
    gtest_discover_tests(msc_common_tests)
endif()
