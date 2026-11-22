# The soak test's logic (M6.2, REQ-SYS-001): the operator that cycles through every fault, and
# the rules that tell a leak or a drift from noise. Plain C++, no Qt, tested with a fake clock;
# console_app --soak and tools/soak_check use it.
add_library(msc_soak STATIC
    src/fault_cycle.cpp
    src/trend.cpp)
add_library(msc::soak ALIAS msc_soak)

target_include_directories(msc_soak PUBLIC include)
target_link_libraries(msc_soak PUBLIC msc::common msc::teleop)
msc_set_warnings(msc_soak)

if(BUILD_TESTING)
    add_executable(msc_soak_tests
        tests/fault_cycle_test.cpp
        tests/trend_test.cpp)
    target_link_libraries(msc_soak_tests PRIVATE msc::soak GTest::gtest_main)
    msc_set_warnings(msc_soak_tests)
    gtest_discover_tests(msc_soak_tests)
endif()
