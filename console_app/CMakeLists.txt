find_package(Qt6 6.8 REQUIRED COMPONENTS Quick)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(console_app src/main.cpp)

qt_add_qml_module(console_app
    URI MiniConsole
    VERSION 1.0
    QML_FILES qml/Main.qml)

target_link_libraries(console_app PRIVATE Qt6::Quick msc::common)
target_compile_definitions(console_app PRIVATE MSC_VERSION="${PROJECT_VERSION}")
set_target_properties(console_app PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
msc_set_warnings(console_app NO_WERROR)
if(MSVC)
    # C4702 (unreachable code) fires inside Qt's own headers after inlining.
    target_compile_options(console_app PRIVATE /wd4702)
endif()

if(BUILD_TESTING)
    # Loads the QML and quits: catches a broken QML module or a missing Qt plugin.
    # Software rendering + offscreen platform so it runs on CI machines without a GPU.
    add_test(NAME console_app.smoke COMMAND console_app --smoke)
    set_tests_properties(console_app.smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
endif()
