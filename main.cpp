#include "modules.hpp"
#include <stdexcept>
#include <sstream>
#include <hyprland/src/debug/HyprCtl.hpp>

namespace {
    bool screenshareStarted = false;
    HANDLE pluginHandle=nullptr;
    SP<SHyprCtlCommand> statusCommand;

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
        statusCommand=HyprlandAPI::registerHyprCtlCommand(handle,{.name="hdrfix",.exact=false,.fn=[](eHyprCtlOutputFormat,std::string request){
            std::istringstream words(request);std::string name,action;words>>name>>action;
            if(!action.empty() && action!="status")return std::string("error: use hdrfix status");
            return std::string("{\"version\":\"")+HDRFix::VERSION+"\",\"color_fixes\":true,\"alpha_fixes\":true,\"blur_fixes\":true,\"capture_color_fixes\":true}";
        }});
        if(!statusCommand)throw std::runtime_error("cannot register HDR color-fix status");
    } catch (...) {
        cleanup();
        throw;
    }
    return {"hyprland-fix-hdr", "HDR color, alpha, blur and capture-color corrections", "daniel", HDRFix::VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    cleanup();
}
