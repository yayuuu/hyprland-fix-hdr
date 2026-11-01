# hyprland-fix-hdr

HDR rendering, color, blur and screen-capture corrections for stock Hyprland.
Version **0.6.0** targets Hyprland **0.56.2** (`efb50993780079460b0cbed1363e2166a2de1d9f`) and Aquamarine **0.15.x**.

## What it does

### Fixes colors in screen sharing, screenshots and OBS capture

- Converts the HDR monitor mirror into a correctly described SDR/sRGB capture
  image instead of treating HDR/FP16 values as ordinary SDR pixels.
- Uses the monitor's active `sdr_min_luminance` and `sdr_max_luminance` to map
  the captured image, rather than relying on a hardcoded default SDR range.
- Compensates for the monitor's SDR brightness and saturation settings in the
  capture conversion, avoiding applying that display adjustment twice.
- Bypasses Hyprland's problematic HDR unmodified-copy MRT path, which caused
  frozen capture/screenshots and incorrect blur with FP16 framebuffers.
- Keeps normal behavior for monitors outside HDR mode.

This is HDR-to-SDR capture correction: it does not turn an SDR OBS recording
into an HDR recording. OBS's recording color space and encoder remain its own
settings.

### Corrects alpha blending and transparent surfaces in HDR

- Corrects the effective opacity of translucent windows and fading surfaces,
  including window alpha, fade alpha, surface alpha modifiers and overall
  opacity.
- Corrects per-pixel alpha for eligible RGBA textures, in addition to scalar
  window opacity. Fully transparent and fully opaque endpoints are preserved.
- Applies shader and opacity adjustments only for the affected draw, restoring
  temporary state afterwards instead of rewriting the client's surface state.

The HDR alpha correction is
`1 - (1 - alpha)^1.5`. It is a targeted appearance correction, not a replacement
for Hyprland's entire blending/color-management implementation.

### Fixes blur behind transparent windows in HDR

- Creates the SDR blur input from the HDR source with the monitor's luminance
  mapping, then runs the configured blur passes and finish settings.
- Temporarily supplies the SDR/sRGB color description when the blur texture is
  drawn in HDR, avoiding a second incorrect HDR interpretation.
- Retains ordinary blur handling when the HDR correction cannot be used.

### Fullscreen rendering and capture cadence

Eligible opaque fullscreen HDR surfaces can use the existing single-pass rendering path. Capture pacing coalesces requests with genuine content updates while retaining a deadline for stopped clients. Unsupported scenes retain normal rendering.

### Cross-GPU copying

Cross-GPU copying optimizations, the Vulkan transfer worker and scRGB-to-PQ transport conversion have moved to [hyprland-gpu-switch](https://github.com/yayuuu/hyprland-gpu-switch). That plugin handles both SDR and HDR transfers, including configurations that do not require automatic renderer switching. See its README for supported formats, synchronization, requirements and limitations.

This plugin no longer implements or launches the transfer worker. Its single-pass rendering path can use the optional transport service provided by hyprland-gpu-switch; without it, ordinary drawing remains available. HDR color, alpha, blur and capture corrections remain here and work independently.

## Build

Install a C++23 compiler, Make, pkg-config and matching development packages for Hyprland, Aquamarine, Pixman, libdrm, Pango/Cairo, libinput, libudev, Wayland and libxkbcommon. The compositor headers must match the running binary.

```sh
make
```

The shared object no longer embeds a Vulkan worker or requires shader compilation. Load `hyprland-fix-hdr.so` through the Hyprland configuration.

## Controls

```sh
hyprctl hdrfix status
hyprctl hdrfix on
hyprctl hdrfix off
```

`on` and `off` control the fullscreen optimization. Color/capture corrections remain installed. Transport policy and counters belong to its provider; the old offload and worker controls were removed.
