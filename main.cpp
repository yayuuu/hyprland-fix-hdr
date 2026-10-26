#include "modules.hpp"
#include <stdexcept>

namespace {
    bool screenshareStarted = false;
    bool offloadStarted = false;

    void cleanup() {
        // Offload rendering can use the color/capture hooks: remove it first.
        if (offloadStarted) {
            offloadStarted = false;
            HDRFix::exitOffload();
        }
        if (screenshareStarted) {
            screenshareStarted = false;
            HDRFix::exitScreenshare();
        }
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    const std::string serverHash = __hyprland_api_get_hash();
    const std::string clientHash = __hyprland_api_get_client_hash();
    if (serverHash != clientHash)
        throw std::runtime_error("hyprland-fix-hdr: Hyprland headers/runtime ABI mismatch");
    try {
        // Mark before init so partially installed hooks/timers are cleaned too.
        screenshareStarted = true;
        HDRFix::initScreenshare(handle);
        offloadStarted = true;
        HDRFix::initOffload(handle);
    } catch (...) {
        cleanup();
        throw;
    }
    return {"hyprland-fix-hdr", "HDR color/capture fixes, demand-driven NVIDIA offload and capture/VRR synchronization", "daniel", HDRFix::VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    cleanup();
}
