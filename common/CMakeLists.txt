# Plain C++ shared by arm_sim and console_app. No Qt here, on purpose.
add_library(msc_common STATIC
    src/crc32.cpp)
add_library(msc::common ALIAS msc_common)

target_include_directories(msc_common PUBLIC include)
target_compile_features(msc_common PUBLIC cxx_std_20)
msc_set_warnings(msc_common)

if(BUILD_TESTING)
    add_executable(msc_common_tests
        tests/crc32_test.cpp
        tests/clock_test.cpp)
    target_link_libraries(msc_common_tests PRIVATE msc::common GTest::gtest_main)
    msc_set_warnings(msc_common_tests)
    gtest_discover_tests(msc_common_tests)
endif()
