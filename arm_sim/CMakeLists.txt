# The simulated robot arm: its own process, plain C++, no Qt (ADR 0001).
# The controller and model are a library, so the tests drive them without the process.
add_library(msc_arm STATIC
    src/arm_model.cpp
    src/arm_controller.cpp)
add_library(msc::arm ALIAS msc_arm)
target_include_directories(msc_arm PUBLIC include)
target_link_libraries(msc_arm PUBLIC msc::common msc::comms)
msc_set_warnings(msc_arm)

add_executable(arm_sim src/main.cpp)
target_link_libraries(arm_sim PRIVATE msc::arm)
target_compile_definitions(arm_sim PRIVATE MSC_VERSION="${PROJECT_VERSION}")
msc_set_warnings(arm_sim)

if(BUILD_TESTING)
    add_executable(msc_arm_tests
        tests/arm_model_test.cpp
        tests/arm_controller_test.cpp)
    target_link_libraries(msc_arm_tests PRIVATE msc::arm GTest::gtest_main)
    msc_set_warnings(msc_arm_tests)
    gtest_discover_tests(msc_arm_tests)

    add_test(NAME arm_sim.version COMMAND arm_sim --version)
    set_tests_properties(arm_sim.version PROPERTIES
        PASS_REGULAR_EXPRESSION "arm_sim ${PROJECT_VERSION}")

    # The real process for a second on a free port: it must start, run its loop and report.
    add_test(NAME arm_sim.runs_its_loop COMMAND arm_sim --listen=127.0.0.1:0 --duration=1 --report)
    set_tests_properties(arm_sim.runs_its_loop PROPERTIES
        PASS_REGULAR_EXPRESSION "ticks .* overruns"
        TIMEOUT 30)
endif()
