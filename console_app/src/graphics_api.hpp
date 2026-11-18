#pragma once

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
