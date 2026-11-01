#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace HDRFix {
    inline constexpr const char* VERSION = "0.9.1";
    void initScreenshare(HANDLE handle);
    void exitScreenshare();
    std::string screenshareStatus();
}
