#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace HDRFix {
    inline constexpr const char* VERSION = "0.6.0";
    void initScreenshare(HANDLE handle);
    void exitScreenshare();
    void initOffload(HANDLE handle);
    void exitOffload();
}
