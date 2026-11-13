# The simulated robot arm: its own process, plain C++, no Qt.
add_executable(arm_sim src/main.cpp)
target_link_libraries(arm_sim PRIVATE msc::common)
target_compile_definitions(arm_sim PRIVATE MSC_VERSION="${PROJECT_VERSION}")
msc_set_warnings(arm_sim)

if(BUILD_TESTING)
    add_test(NAME arm_sim.version COMMAND arm_sim --version)
    set_tests_properties(arm_sim.version PROPERTIES
        PASS_REGULAR_EXPRESSION "arm_sim ${PROJECT_VERSION}")
endif()
