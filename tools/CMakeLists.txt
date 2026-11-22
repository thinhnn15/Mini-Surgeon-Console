# End-to-end checks with real processes: arm_sim started, killed, faulted, for real.
if(BUILD_TESTING)
    # REQ-SAF-001: start arm_sim and a console, kill the console while the arm moves, and check
    # that the arm holds within 3 heartbeat periods and stays stopped.
    add_executable(link_check link_check.cpp child_process.hpp)
    target_link_libraries(link_check PRIVATE msc::comms)
    msc_set_warnings(link_check)
    add_test(NAME arm_sim.REQ_SAF_001_HoldsWhenTheConsoleDies
             COMMAND link_check --arm-exe=$<TARGET_FILE:arm_sim>)
    set_tests_properties(arm_sim.REQ_SAF_001_HoldsWhenTheConsoleDies PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS"
        TIMEOUT 60)

    # REQ-SAF-002 / M4.4: every fault the injector can cause, with arm_sim as a real process:
    # Fault with the right code, the arm holds, reset refused until the cause is gone, then Idle.
    add_executable(safety_check safety_check.cpp child_process.hpp)
    target_link_libraries(safety_check PRIVATE msc::teleop)
    msc_set_warnings(safety_check)
    add_test(NAME console.REQ_SAF_002_EveryFaultEndsInASafeState
             COMMAND safety_check --arm-exe=$<TARGET_FILE:arm_sim>)
    set_tests_properties(console.REQ_SAF_002_EveryFaultEndsInASafeState PROPERTIES
        PASS_REGULAR_EXPRESSION "\nPASS"
        FAIL_REGULAR_EXPRESSION "FAIL"
        TIMEOUT 120)

    # M6.2 / REQ-SYS-001: a long session with arm_sim and console_app as real processes, every
    # fault injected in turn, memory, handles and latency measured from outside. Registered as a
    # CTest in console_app/CMakeLists.txt (it needs the console and a display).
    add_executable(soak_check soak_check.cpp child_process.hpp)
    target_link_libraries(soak_check PRIVATE msc::comms msc::soak)
    if(WIN32)
        target_link_libraries(soak_check PRIVATE psapi)
    endif()
    msc_set_warnings(soak_check)

    # M6.1: every requirement traced to the tests that carry its ID, every risk to tests that
    # exist, and docs/traceability.md up to date. Reads the sources, not CTest's list, which
    # differs between machines (the screen checks are registered only where there is a display).
    add_library(msc_trace STATIC trace.cpp trace.hpp)
    target_include_directories(msc_trace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    msc_set_warnings(msc_trace)

    add_executable(trace_check trace_check.cpp)
    target_link_libraries(trace_check PRIVATE msc_trace)
    msc_set_warnings(trace_check)
    add_test(NAME trace_check.EveryRequirementAndRiskIsTracedToTests
             COMMAND trace_check --source-dir=${PROJECT_SOURCE_DIR})
    set_tests_properties(trace_check.EveryRequirementAndRiskIsTracedToTests PROPERTIES
        PASS_REGULAR_EXPRESSION "\nPASS"
        FAIL_REGULAR_EXPRESSION "FAIL"
        TIMEOUT 30)

    add_executable(msc_trace_tests tests/trace_test.cpp)
    target_link_libraries(msc_trace_tests PRIVATE msc_trace GTest::gtest_main)
    msc_set_warnings(msc_trace_tests)
    gtest_discover_tests(msc_trace_tests)
endif()
