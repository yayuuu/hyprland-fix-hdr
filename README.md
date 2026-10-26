# hyprland-fix-hdr

Hyprland plugin for HDR rendering, PRIME render offload, screen capture and
frame synchronization. Version **0.5.0** extends the plugin's HDR fixes with
render offload optimizations and capture synchronization. It targets Hyprland **0.56.2**
ABI (`efb50993780079460b0cbed1363e2166a2de1d9f`) and Aquamarine **0.15.x**.

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

### Optimizes PRIME render offload for HDR games

- Moves the expensive linear FP16/scRGB-to-BT.2020/PQ RGB10 conversion onto the
  NVIDIA render GPU before the display GPU samples the frame. This reduces the
  display GPU's cost of reading and converting cross-GPU FP16 game buffers.
- Starts conversion on a source commit, so NVIDIA can process a frame before
  the compositor needs it. Matching source generations and native fences
  protect input acquisition, conversion, output sampling and buffer reuse.
- Prefers a device-local input mapping when the exported DMA-BUF permits it.
  Import mapping selection does not change the producer's buffer allocation.
- Automatically requests NVIDIA-local input buffers after ordinary offload
  has produced a usable frame. Conversion stays in NVIDIA VRAM; only packed
  PQ10 pixels are downloaded to shared memory. Protected input textures keep
  the display GPU from importing the original NVIDIA VRAM allocation.
- Preserves each source buffer's native layout through overview rendering and
  window resizing, including padded row pitches. Layout selection uses the
  original buffer instead of a temporarily replaced render-pass texture.
- Overlaps FP16 upload, PQ conversion and packed output transfer in row bands
  on separate GPU queues when supported. Per-band GPU semaphores preserve
  ordering; the display GPU waits for completion of the entire output frame.
- Supports fullscreen, ordinary windows, resizing and scrolloverview previews,
  including while whole-monitor OBS capture is active. Surface replacement is
  limited to the queued render pass; client color metadata is preserved.
- Reuses three output buffers, cached source views and reserved output capacity
  across ordinary window resizes. Transitions do not deliberately insert black
  frames or freeze a previous image. Unsupported or larger layouts fall back to
  normal rendering while resources are prepared.
- Uses a strict fullscreen single-pass framebuffer bypass when safe. Active
  capture keeps ordinary composition consistent while retaining HDR offload.
- Keeps Vulkan/NVIDIA ownership in a separate **on-demand helper process**.
  Only eligible opaque linear FP16 HDR buffers from a process with exactly
  `__NV_PRIME_RENDER_OFFLOAD=1` qualify. SDR windows and normal desktop use do
  not start the helper. It stops after 500 ms without an eligible draw, on
  disable, window close and plugin unload.
- Sends the packed result through host-visible system RAM. The display GPU
  never imports the helper's NVIDIA-local output VRAM.

The offload/fullscreen path currently targets a single output, and one HDR
source owns the offload helper at a time. Unsupported alpha/ICC/layout combinations
keep Hyprland's normal rendering; offload is not a universal SDR acceleration
path. NVIDIA is not selected as the compositor's permanent rendering device.

### Improves capture synchronization and VRR behavior

- Coalesces capture-only damage with the next real content update, reducing
  repeated presentations of an old game frame just because OBS requested a
  buffer. Existing damage from windows, overlays and the cursor is preserved.
- Tracks recently drawn content per monitor. This synchronization also works
  for **SDR games**, independently of HDR eligibility and the NVIDIA helper.
- Uses a 25 ms fallback deadline if content stops updating, so a pending capture
  is not left waiting indefinitely. First/unsupported captures keep the normal
  path; unique-owned frame lifetimes are checked on the compositor event loop.
- Keeps the composition path consistent throughout an active capture session,
  instead of alternating fullscreen bypass and the work-buffer path between
  individual capture requests.
- Repairs stale VRR capability caches when DRM reports the connector as VRR
  capable but Aquamarine's cached flag says otherwise. It does not enable VRR
  on hardware that DRM does not report as capable.

These improvements do not impose a fixed game FPS cap or force the monitor
to run at its maximum refresh rate. Normal desktop/window/overview animations
may still need extra draws, and source/GPU timing variation remains.

### Handles DMA-BUF import failures more safely

Checks DMA-BUF dimensions, format, planes, descriptors and strides, and probes
EGL image import before creating a renderbuffer. Unsupported imports return
failure through the existing path. Successful imports are cached with their
layout and buffer identity, with a bounded cache size.

## Build and load

```sh
make
```

Load the plugin in your Hyprland configuration:

```lua
hl.plugin.load("/absolute/path/to/hyprland-fix-hdr/hyprland-fix-hdr.so")
```

The worker executable and compute shader are embedded in the shared object.
The worker runs as a separate on-demand process, launched from an anonymous
memory file. A C++ supervisor handles its logging, signals and process cleanup.

The build uses a C++ compiler, matching Hyprland/Aquamarine development headers,
Vulkan development files, `glslangValidator` and binutils. Intermediate
resources stay in `build/`. The worker initializes Vulkan/NVIDIA independently
of the compositor. Exact Hyprland/Aquamarine ABI checks happen at build/load
time. Builds replace the shared object atomically without truncating a library
already mapped by the compositor.

NVIDIA preprocessing requires `__NV_PRIME_RENDER_OFFLOAD=1` in the actual game
process's environment. Configure HDR and GPU selection through your launcher.
Normal desktop use without an eligible game must leave no helper running, so
the NVIDIA card can be handed back to VFIO when its other users also release it.

## Control and diagnostics

```sh
hyprctl hdrfix
hyprctl hdrfix offload-off
hyprctl hdrfix offload-on
```

`offload-off` stops NVIDIA preprocessing but retains the other fixes.
`off` disables offload/fullscreen optimizations; capture colors, alpha/blur
fixes and independent capture synchronization remain enabled.
Unloading the plugin removes its hooks and timers, drains GPU work, releases
retained buffers and stops the helper. Initialization cleans up resources if
a hook or command registration fails partway through.

`capture-pacing-on/off`, `uniform-capture-on/off`, `eager-on/off` and
`pipeline-on/off` and `local-input-on/off` allow
diagnostic comparisons. `vram-input-auto` restores the default automatic
NVIDIA-local input policy. It activates only after a qualifying HDR source has
started the helper and produced a usable frame. `vram-input-off` disables that
policy and restores ordinary allocation feedback. The client must replace its
existing buffers before the transition completes.

For explicit layout comparisons, `vram-input-on` requests linear VRAM input and
`vram-input-native-on` selects native NVIDIA image layout. Both override automatic
selection. These layouts are not interchangeable: VRAM residency alone does not
identify an image's layout. Native input currently relies on the compatible
NVIDIA producer/driver layout verified on the hardware listed below. Restore ordinary input and allow buffers to
retire before unloading the plugin. A stopped helper retains the last converted
image for protected inputs until a usable source returns.

`early-release-on/off` and
`cached-capture-on/off` are **experimental options disabled by default**.

Status reports the owning PID/offload flag, worker/logger PIDs, readiness,
conversion counts and capture coalescing/timeouts.
It also reports whether pipelined transfers are requested and active.
`worker-profile` records a bounded GPU timing sample in the helper log;
with pipelining, individual stage intervals overlap and must not be added.
`last_reason` describes the fullscreen bypass, so a fallback there does not
mean offload stopped. Conversion/render counts alone do not prove distinct
presented game FPS. `trace-on`, `trace-off`, `trace-read` record bounded timing
traces. The `nvidia_gpu_fds` count can include imports owned by other clients;
helper PIDs and process exit are checked separately for idle behavior.

## Tested hardware

- **CPU:** AMD Ryzen 7 7800X3D.
- **Render GPU:** NVIDIA GeForce RTX 4070, used for PRIME render offload and HDR
  preprocessing.
- **Display GPU:** Intel Arc A380, driving the monitor and used for composition,
  capture and hardware encoding.
- **Display capabilities tested:** HDR and VRR.

Other hardware combinations have not been validated.
