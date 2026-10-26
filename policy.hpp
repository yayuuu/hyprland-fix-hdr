#pragma once
#include <string_view>

namespace Singlepass {
    struct SConditions {
        bool enabled = false;
        bool targetOutput = false;
        bool normalGLFrame = false;
        bool hdrFP16 = false;
        bool noCapture = false;
        bool noPostprocessing = false;
        bool oneOpaqueFullscreenSurface = false;
        bool fullDamage = false;
        bool identityInputConversion = false;
    };

    constexpr std::string_view skipReason(const SConditions& c) {
        if (!c.enabled)
            return "disabled";
        if (!c.targetOutput)
            return "other output";
        if (!c.normalGLFrame)
            return "not a normal offloaded GL frame";
        if (!c.hdrFP16)
            return "output is not HDR with an FP16 work buffer";
        if (!c.noCapture)
            return "capture or mirroring requires the work buffer";
        if (!c.noPostprocessing)
            return "postprocessing, transform or non-shader CM";
        if (!c.oneOpaqueFullscreenSurface)
            return "frame contains overlays, cursor or non-opaque surfaces";
        if (!c.fullDamage)
            return "frame does not repaint the complete output";
        if (!c.identityInputConversion)
            return "input-to-workbuffer conversion is not an HDR identity";
        return {};
    }
}
