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
endif()
