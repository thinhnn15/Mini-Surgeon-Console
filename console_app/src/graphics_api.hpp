#pragma once

#include <rhi/qrhi.h>
#include <QByteArray>
#include <QSGRendererInterface>

// The graphics API's name, for the reports printed by --check-video and --measure-latency.
inline const char* graphicsApiName(QSGRendererInterface::GraphicsApi api) noexcept {
    switch (api) {
        case QSGRendererInterface::Direct3D11:
            return "Direct3D 11";
        case QSGRendererInterface::Direct3D12:
            return "Direct3D 12";
        case QSGRendererInterface::Vulkan:
            return "Vulkan";
        case QSGRendererInterface::Metal:
            return "Metal";
        case QSGRendererInterface::OpenGL:
            return "OpenGL";
        case QSGRendererInterface::Software:
            return "software";
        default:
            return "unknown";
    }
}

// A graphics device that is really the CPU (Windows' Basic Render Driver on a CI runner without a
// GPU, Mesa's llvmpipe, SwiftShader). Drawing the window there takes the cores the arm link and
// the teleop core need (journal, M5.4). Qt does not always say so in the device type (it did not
// on the GitHub Windows runner), so the software devices are also known by name.
inline bool drawsOnTheCpu(const QRhiDriverInfo& info) {
    const QByteArray name = info.deviceName.toLower();
    return info.deviceType == QRhiDriverInfo::CpuDevice || name.contains("basic render") ||
           name.contains("warp") || name.contains("llvmpipe") || name.contains("swiftshader");
}
