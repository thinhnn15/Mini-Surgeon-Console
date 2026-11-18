find_package(Qt6 6.8 REQUIRED COMPONENTS Quick ShaderTools)
# Only the camera source needs Qt Multimedia (M2.5). Without it the console still builds and
# runs on the synthetic source.
find_package(Qt6 6.8 QUIET OPTIONAL_COMPONENTS Multimedia)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(console_app
    src/main.cpp
    src/freeze_check.hpp
    src/freeze_check.cpp
    src/graphics_api.hpp
    src/latency_report.hpp
    src/latency_report.cpp
    src/video_check.hpp
    src/video_check.cpp)

if(TARGET Qt6::Multimedia)
    target_sources(console_app PRIVATE src/camera_source.hpp src/camera_source.cpp)
    target_link_libraries(console_app PRIVATE Qt6::Multimedia)
    target_compile_definitions(console_app PRIVATE MSC_HAVE_CAMERA=1)
else()
    message(STATUS "console_app: Qt Multimedia not found, building without the camera source")
    target_compile_definitions(console_app PRIVATE MSC_HAVE_CAMERA=0)
endif()

qt_add_qml_module(console_app
    URI MiniConsole
    VERSION 1.0
    QML_FILES qml/Main.qml
    SOURCES src/video_item.hpp src/video_item.cpp src/teleop_model.hpp src/teleop_model.cpp)

# Vulkan-style GLSL compiled at build time by qsb into one .qsb per shader, holding SPIR-V,
# HLSL, MSL and GLSL variants, so the same file serves every QRhi backend.
# Served from the resource system as :/shaders/<name>.qsb.
qt_add_shaders(console_app "video_shaders"
    PREFIX "/"
    FILES
        shaders/video.vert
        shaders/video.frag)

target_include_directories(console_app PRIVATE src)
# GuiPrivate: QRhi (<rhi/qrhi.h>) is public API since Qt 6.6 but, with a weaker compatibility
# promise than the rest of Qt, still lives in the private module.
target_link_libraries(console_app PRIVATE Qt6::Quick Qt6::GuiPrivate msc::common msc::video msc::comms msc::teleop)
target_compile_definitions(console_app PRIVATE MSC_VERSION="${PROJECT_VERSION}")
set_target_properties(console_app PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
msc_set_warnings(console_app NO_WERROR)
if(MSVC)
    # C4702 (unreachable code) fires inside Qt's own headers after inlining.
    target_compile_options(console_app PRIVATE /wd4702)
endif()

if(BUILD_TESTING)
    # Every test runs against the Qt it was built with: its DLLs on PATH, and its plugins. The
    # plugin path matters once windeployqt has copied Qt next to the exe (to run it by
    # double-click): Qt then only looks for plugins there, and windeployqt copies the "windows"
    # platform plugin but not "offscreen", which the smoke tests use. Without this they die in
    # qFatal, and a Debug build shows a modal dialog that blocks the test run.
    set(qt_test_env
        "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
        "QT_PLUGIN_PATH=set:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}")

    # Loads the QML and quits: catches a broken QML module or a missing Qt plugin.
    # Software rendering + offscreen platform so it runs on CI machines without a GPU.
    # (The video item draws nothing there, which is fine: this test is about start-up.)
    add_test(NAME console_app.smoke COMMAND console_app --smoke)
    set_tests_properties(console_app.smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        ENVIRONMENT_MODIFICATION "${qt_test_env}")

    # --source camera when the camera asked for is not there: must start on the pattern, not
    # crash or quit. A name no camera has, so it behaves the same with or without a webcam.
    add_test(NAME console_app.camera_missing_falls_back
             COMMAND console_app --smoke --source camera --camera=no-such-camera-msc)
    set_tests_properties(console_app.camera_missing_falls_back PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        ENVIRONMENT_MODIFICATION "${qt_test_env}")

    # Shows the real window with a real GPU backend and reads the frame number back from the
    # screen. Needs a display: always there on Windows (CI runners included, via the basic
    # render driver); on Linux only when configured from a desktop session, so headless CI
    # skips it.
    if(WIN32 OR DEFINED ENV{DISPLAY} OR DEFINED ENV{WAYLAND_DISPLAY})
        add_test(NAME console_app.REQ_VID_001_NewestFrameReachesTheScreen COMMAND console_app --check-video)
        set_tests_properties(console_app.REQ_VID_001_NewestFrameReachesTheScreen PROPERTIES
            ENVIRONMENT_MODIFICATION "${qt_test_env}"
            TIMEOUT 60)

        # REQ-VID-002: runs the real window for a few seconds and must report capture-to-present
        # p50 and p99. The numbers themselves depend on the machine; they go in the journal.
        add_test(NAME console_app.REQ_VID_002_LatencyIsMeasuredAndReported
                 COMMAND console_app --measure-latency=3)
        set_tests_properties(console_app.REQ_VID_002_LatencyIsMeasuredAndReported PROPERTIES
            ENVIRONMENT_MODIFICATION "${qt_test_env}"
            TIMEOUT 60)

        # REQ-VID-003: stops the source the way an unplugged camera stops, and checks that the
        # picture on screen is flagged as frozen in time, then live again after a restart.
        add_test(NAME console_app.REQ_VID_003_FrozenVideoIsFlagged COMMAND console_app --check-freeze)
        set_tests_properties(console_app.REQ_VID_003_FrozenVideoIsFlagged PROPERTIES
            ENVIRONMENT_MODIFICATION "${qt_test_env}"
            TIMEOUT 60)
    endif()
endif()
