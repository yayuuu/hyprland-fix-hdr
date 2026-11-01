#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace HDRFix {
    inline constexpr const char* VERSION = "0.7.0";
    void initScreenshare(HANDLE handle);
    void exitScreenshare();
}
