find_package(Qt6 6.8 REQUIRED COMPONENTS Quick ShaderTools)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(console_app
    src/main.cpp
    src/graphics_api.hpp
    src/latency_report.hpp
    src/latency_report.cpp
    src/video_check.hpp
    src/video_check.cpp)

qt_add_qml_module(console_app
    URI MiniConsole
    VERSION 1.0
    QML_FILES qml/Main.qml
    SOURCES src/video_item.hpp src/video_item.cpp)

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
target_link_libraries(console_app PRIVATE Qt6::Quick Qt6::GuiPrivate msc::common msc::video)
target_compile_definitions(console_app PRIVATE MSC_VERSION="${PROJECT_VERSION}")
set_target_properties(console_app PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
msc_set_warnings(console_app NO_WERROR)
if(MSVC)
    # C4702 (unreachable code) fires inside Qt's own headers after inlining.
    target_compile_options(console_app PRIVATE /wd4702)
endif()

if(BUILD_TESTING)
    set(qt_bin_path "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

    # Loads the QML and quits: catches a broken QML module or a missing Qt plugin.
    # Software rendering + offscreen platform so it runs on CI machines without a GPU.
    # (The video item draws nothing there, which is fine: this test is about start-up.)
    add_test(NAME console_app.smoke COMMAND console_app --smoke)
    set_tests_properties(console_app.smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        ENVIRONMENT_MODIFICATION "${qt_bin_path}")

    # Shows the real window with a real GPU backend and reads the frame number back from the
    # screen. Needs a display: always there on Windows (CI runners included, via the basic
    # render driver); on Linux only when configured from a desktop session, so headless CI
    # skips it.
    if(WIN32 OR DEFINED ENV{DISPLAY} OR DEFINED ENV{WAYLAND_DISPLAY})
        add_test(NAME console_app.REQ_VID_001_NewestFrameReachesTheScreen COMMAND console_app --check-video)
        set_tests_properties(console_app.REQ_VID_001_NewestFrameReachesTheScreen PROPERTIES
            ENVIRONMENT_MODIFICATION "${qt_bin_path}"
            TIMEOUT 60)

        # REQ-VID-002: runs the real window for a few seconds and must report capture-to-present
        # p50 and p99. The numbers themselves depend on the machine; they go in the journal.
        add_test(NAME console_app.REQ_VID_002_LatencyIsMeasuredAndReported
                 COMMAND console_app --measure-latency=3)
        set_tests_properties(console_app.REQ_VID_002_LatencyIsMeasuredAndReported PROPERTIES
            ENVIRONMENT_MODIFICATION "${qt_bin_path}"
            TIMEOUT 60)
    endif()
endif()
