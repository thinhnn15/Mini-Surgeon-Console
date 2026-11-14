cmake_minimum_required(VERSION 3.25)

project(MiniSurgeonConsole
    VERSION 0.1.0
    DESCRIPTION "A small surgeon console simulator for learning real-time video, tele-operation and safety"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MSC_BUILD_CONSOLE "Build the Qt console app (needs Qt 6.8 or newer)" ON)
option(MSC_WERROR "Treat compiler warnings as errors" OFF)
option(MSC_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(MSC_TSAN "Build with ThreadSanitizer (GCC/Clang); cannot be combined with MSC_SANITIZE" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)

include(CTest)
if(BUILD_TESTING)
    include(GoogleTestDependency)
endif()

add_subdirectory(common)
add_subdirectory(arm_sim)

if(MSC_BUILD_CONSOLE)
    add_subdirectory(console_app)
endif()
