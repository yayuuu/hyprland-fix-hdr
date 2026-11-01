# hyprland-fix-hdr

HDR alpha blending and per-texture SDR capture corrections for stock Hyprland.

## Screen sharing, screenshots and mirrors

The plugin uses Hyprland's native `render:keep_unmodified_copy` mechanism and
capture pipeline. When a rendered HDR texture has a native SDR copy, the SDR
output samples that copy. HDR textures without an SDR copy are mapped individually
with the plugin's luminance mapping, including textures used for blurred backgrounds.
The completed screen is not tonemapped again by the plugin.

The conversion uses the monitor's `sdr_min_luminance` and `sdr_max_luminance`,
compensates for display brightness and saturation adjustments already present in
compositor textures, and preserves premultiplied alpha. SDR textures and isolated
SDR scenes retain normal Hyprland rendering.

Hyprland controls when unmodified copies are created. The plugin does not override
that setting, replace the mirror-copy function, implement its own blur passes or
change buffer imports. These captures remain SDR; OBS's recording color space and
encoder are configured separately.

## Alpha blending

The plugin retains the HDR opacity correction `1 - (1 - alpha)^1.5` for translucent
surfaces and eligible RGBA textures. Temporary changes apply only to the affected
monitor draw. Screensharing, mirrors and capture rendering keep the original
scalar and per-pixel alpha. Fully transparent and opaque endpoints are preserved,
and isolated SDR scenes are excluded.

## Install with hyprpm

```sh
hyprpm add https://github.com/yayuuu/hyprland-fix-hdr.git
hyprpm enable hyprland-fix-hdr
hyprpm reload
```

Run `hyprpm reload` when your Hyprland session starts. Do not also load a
manually built copy of this plugin. Hyprpm builds against headers matching the
running compositor and manages the installed library.

## Build

Install a C++23 compiler, Make, pkg-config and matching development packages for
Hyprland and its dependencies. The compositor headers must match the running binary.

```sh
make
```

Load `hyprland-fix-hdr.so` through the Hyprland configuration.

## Status

```sh
hyprctl hdrfix status
```

The status reports the active corrections, SDR-copy and tonemapping draw counts,
and shader failures. GPU transport, fullscreen rendering and recording optimizations
are provided by [hyprland-gpu-switch](https://github.com/yayuuu/hyprland-gpu-switch).
