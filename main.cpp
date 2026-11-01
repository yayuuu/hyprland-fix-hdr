#include "modules.hpp"
#include <stdexcept>
#include <sstream>
#include <hyprland/src/ipc/s1/S1.hpp>

namespace {
    bool screenshareStarted = false;
    HANDLE pluginHandle=nullptr;
    SP<IPC::Socket1::SCommand> statusCommand;

    void cleanup() {
        if(statusCommand){HyprlandAPI::unregisterHyprCtlCommand(pluginHandle,statusCommand);statusCommand.reset();}
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
    pluginHandle=handle;
    try {
        // Mark before init so partially installed hooks/timers are cleaned too.
        screenshareStarted = true;
        HDRFix::initScreenshare(handle);
        statusCommand=HyprlandAPI::registerHyprCtlCommand(handle,{.name="hdrfix",.match=IPC::Socket1::COMMAND_MATCH_PREFIX,.handler=[](const IPC::Socket1::SRequest& request) -> IPC::Socket1::SResponse {
            std::istringstream words(request.command);std::string name,action;words>>name>>action;
            if(!action.empty() && action!="status")return std::string("error: use hdrfix status");
            return HDRFix::screenshareStatus();
        }});
        if(!statusCommand)throw std::runtime_error("cannot register HDR color-fix status");
    } catch (...) {
        cleanup();
        throw;
    }
    return {"hyprland-fix-hdr", "HDR alpha and per-texture SDR capture corrections", "daniel", HDRFix::VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    cleanup();
}
