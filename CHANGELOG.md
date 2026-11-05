# Changelog

All notable changes to **EdgeScale** (`edgerun` / legacy `xfsr-run`) are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/).


## [Unreleased]

## [0.10.0] — 2026-11-04

### Added

- **VRAM Capacity Spoofing & Oversubscription Fallback** (`--spoof-vram [SIZE]` / `EDGESCALE_SPOOF_VRAM`):
  - Intercepts `vkGetPhysicalDeviceMemoryProperties` and `vkGetPhysicalDeviceMemoryProperties2` to report expanded `DEVICE_LOCAL` VRAM capacity (e.g. 8GB, 12GB, 16GB, or `auto`).
  - Transparently intercepts `VK_EXT_memory_budget` (`VkPhysicalDeviceMemoryBudgetPropertiesEXT`) so modern engines (DXVK / VKD3D-Proton / Unreal) see increased VRAM budget instead of locking out High/Ultra textures.
  - Implements graceful allocation spilling in `vkAllocateMemory`: when physical local VRAM is exhausted (`VK_ERROR_OUT_OF_DEVICE_MEMORY`), automatically falls back to allocating over PCIe in system RAM (`HOST_VISIBLE`) instead of crashing the application.
  - Profile key: `spoof_vram=8GB`.
- **Supersampling / DSR (4K Render on 1080p Display)** (`--dsr 4k|1440p|<val>` / `--render 3840x2160 --output 1920x1080`):
  - Enables true downsampling / Virtual Super Resolution (DSR / SSAA) in the Vulkan layer: the game renders internally at 4K (3840×2160) or 1440p, while the swapchain presents at native monitor resolution (e.g. 1080p).
  - Shipped dedicated `edgeDownsample` compute shader in `shaders/edge_pass.comp.glsl`: 4-tap staggered bilinear box filter covering up to a 4×4 texel area, combined with optional Contrast-Adaptive Sharpening (CAS) for clean, anti-aliased SSAA without shimmering.
  - Automatic positive mip LOD bias calculation (`-log2(scale) = +1.0` at 4K→1080p) to match high-resolution texture sampling to display pixel density and eliminate texture crawling.
  - Profile key: `dsr=4k`.

### Fixed

- **Steam Overlay / Companion iGPU Probe Crash (`SIGSEGV` in CS2 / Steam titles)**:
  - Fixed a segmentation fault during companion iGPU initialization in games running with the Steam Overlay and Steam Fossilize layers (`libVkLayer_steam_fossilize.so`).
  - Corrected `layerGIPA` in `DeviceContext::createLogicalViaLoader`: the layer was previously forwarding `pfnNextGetInstanceProcAddr` into `vkLayerCreateDevice` rather than its own negotiated `vkGetInstanceProcAddr`. This prevented the Vulkan loader from identifying EdgeScale in the layer chain and caused the loader to re-enter upstream layers (`steamoverlayvulkanlayer.so`), invoking an uninitialized function pointer.
  - Added a POSIX signal trap (`sigsetjmp` / `sigaction`) around companion device discovery to isolate any third-party layer or driver faults, ensuring seamless fallback to same-GPU upscaling instead of terminating the game.
- **OpenGL / Zink Bidirectional Viewport & Downsampling Remapping**:
  - Fixed quarter-screen / bottom-left postage stamp rendering bug in `gl/xfsr_gl_shim.c` when downsampling or supersampling (DSR 4K / 1440p).
  - Replaced unidirectional `render < window` assumptions across `glViewport`, `glScissor`, `glBlitFramebuffer`, `glfwGetFramebufferSize`, `SDL_GL_GetDrawableSize`, and X11 window queries with bidirectional delta checks (`abs(...) > threshold`).
  - Allowed downsampling scale factors down to `0.125f` (e.g. `scale=0.50` for 4K → 1080p SSAA) in `target_render_size`, `load_extents`, and `shrink_wl_size`.

## [0.9.0] — 2026-11-02

### Added

- **Linux AppImage**: portable `EdgeScale-*-x86_64.AppImage` release artifact ([`scripts/package-appimage.sh`](scripts/package-appimage.sh)) with offline `--install` and `--uninstall` support; primary Linux install path replacing the curl installer.
- **Windows Setup.exe**: Inno Setup installer/uninstaller ([`packaging/windows/edgescale.iss`](packaging/windows/edgescale.iss)) with icon integration, registry setup, and Add/Remove Programs support; ZIP + `.bat` scripts remain as fallback.
- **Edge Nearest Neighbor mode** (`--edge-mode nearest`): EdgePass compute shader point-sampling mode dedicated for 2D pixel art games, preserving crisp sprite pixels with zero blur or haloing. F2 hotkey cycles `quality` → `perf` → `nearest` when Edge upscaler is active.
- **Nearest-neighbor upscaler** (`--upscaler nearest`): `VK_FILTER_NEAREST` blit for 2D pixel art; F3 cycle includes `blit → nearest → nis → edge`.
- **Manual resolutions**: `--render WxH` for absolute internal size; `--output WxH` auto-detects the primary display when omitted (supports 4K). Profile keys `output=` / `render=`. Adaptive scale max raised to 4.0×.

### Changed

- README refreshed for EdgeScale branding (`EDGESCALE_*` env vars primary), current one-way/fused offload guidance, AppImage + Setup.exe install, nearest + 4K docs.
- **Install script rename**: [`scripts/install-edge-run.sh`](scripts/install-edge-run.sh) replaces `install-xfsr-run.sh` (`install-edgerun.sh` remains a symlink). Docs and CMake `install-edge-run` target updated; user-facing copy no longer leads with proprietary FSR branding.
- **Godot 3 GLES (BunnyForest)**: auto-enable Zink with caps-shrink so the overlay and live upscaler/preset changes work; GL shim remaps default-framebuffer viewports only (keeps design RTs intact) and does **not** X11 size-lie when the exe links `libX11` (avoids maximize/fullscreen postage-stamps).
- **Godot 4 sowrap + Zink (MMX)**: live F1 preset recreate is blocked (toast: restart `edgerun` with `--preset`); upscaler hotkeys still apply live.

### Fixed

- **Cross-device swapchain re-creation & offload safety**:
  - Prevented crash (`SIGSEGV` in mesa/radv) when toggling between offload (iGPU swapchain) and same-GPU (dGPU swapchain) modes by properly tracking owning device and avoiding invalid `oldSwapchain` handle reuse across physical devices.
  - Guarded `F4` offload hotkey in games configured with `offload=0` via profile or env var (e.g. D3D12 / VKD3D titles like Resident Evil Village) to avoid accidental runtime presentation lockups.
- **Fullscreen Upscaler Switching**:
  - Live upscaler hotkeys (`F3` cycle between Edge, NIS, FSR1, Nearest, Blit) now update and run immediately in Fullscreen mode. Previously, `xfsr_QueuePresentKHR` guarded live switching behind `ps->renderW != ps->displayW`, which froze upscaler state whenever the title ran at native display resolution.
  - Passes are lazily initialized and resized upon activation, ensuring live hotkey responsiveness regardless of whether scaling or 1:1 post-processing sharpening is in use.
  - Fixed `caps_shrink=0` (in-game resolution titles like Resident Evil Village) falsely taking the identity passthrough bypass when entering Fullscreen at lower resolutions due to compositor surface query matching.
- **Zink image not filling the window**:
  - Avoid monitor-sized `DISPLAY_EXTENT` unless `--output` / `--render` / gamescope is set; CreateSwapchain ignores mismatched monitor seeds for windowed Zink.
  - GL shim proportionally remaps letterboxed viewports/scissors/blits (Godot keep-aspect), keeps SDL/GLFW window and drawable size queries in sync at render resolution, and preserves `GL_NEAREST` on remapped blits when `--upscaler nearest`.
- **BunnyForest maximize/fullscreen stamp**: X11 size-lie desynced Godot from WSI after resize; lie auto-off for `libX11`-linked titles; drawable-size queries stay real so stretch tracks the window.

## [0.8.2] — 2026-11-01

### Added

- **Windows Native Vulkan Support**:
  - Added native Windows x86_64 implicit Vulkan layer support (`VkLayer_XFSR.dll`) exporting `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`, and `vkNegotiateLoaderLayerInterfaceVersion`.
  - In-game Win32 hotkey polling using `GetAsyncKeyState` (`F1` preset, `F2` RCAS/edge-mode, `F3` upscaler cycle, `F10` pin).
  - Win32 memory alignment and platform abstractions (`_aligned_malloc`, `_putenv_s`, `%LOCALAPPDATA%\EdgeScale` shader resolution, and `%TEMP%` control files).
  - Windows batch build & installation scripts ([`scripts/build-windows.bat`](scripts/build-windows.bat), [`scripts/install-windows.bat`](scripts/install-windows.bat), and [`scripts/uninstall-windows.bat`](scripts/uninstall-windows.bat)).
  - Standalone Linux-to-Windows cross-compilation pipeline ([`scripts/build-windows-cross.sh`](scripts/build-windows-cross.sh)).
  - Automated Windows packaging and release artifact generation in GitHub Actions (`.github/workflows/release.yml`).
- **Technical Documentation & Branding**:
  - Comprehensive architectural deep-dive and comparison with Lossless Scaling and Magpie ([`docs/ARCHITECTURE_AND_COMPARISON.md`](docs/ARCHITECTURE_AND_COMPARISON.md)).
  - Integrated official project logo into README and updated all project links to `https://github.com/jersobh/edge`.

## [0.8.1] — 2026-11-01

### Changed

- **Rebranded to EdgeScale (`edgerun`)**:
  - The project is now **EdgeScale**, putting the custom Edge reconstruction engine and in-process scaling pipeline front and center.
  - The primary CLI runner is now **`edgerun`** (e.g. `edgerun --preset performance %command%`).
  - Full backward compatibility: `xfsr-run` is maintained as a direct alias/symlink, and all `XFSR_*` environment variables and configs (`~/.config/xfsr/profiles.conf`) continue to work alongside new `EDGESCALE_*` / `~/.config/edgescale/` paths.

### Fixed

- **CS2 crash on startup with iGPU offload (NVIDIA Reflex)**: CS2 enables Reflex (`VK_NV_low_latency2`) and passed the opaque one-way swapchain handle straight to the NVIDIA ICD (causing a `libnvidia-glcore` SIGSEGV). The layer now intercepts `vkSetLatencySleepModeNV`, `vkLatencySleepNV` (signals the sleep timeline semaphore), `vkSetLatencyMarkerNV`, `vkGetLatencyTimingsNV`, `vkGetRefreshCycleDurationGOOGLE` and `vkSetLocalDimmingAMD`, stubbing them for proxy handles and forwarding real ones.
- **`--preset` ignored at startup**: when caps-shrink is active, an engine's initial swapchain size (e.g. CoD4 starting windowed at 1280x720 on a 1080p display) no longer overwrites the chosen preset/scale back to `quality`. Ratio-derived scale is only used with `XFSR_CAPS_SHRINK=0`.

## [0.8.0] — 2026-10-31

### Fixed

- **Edge upscaler colors**: edge output now matches FSR1 / NIS / blit.
  - Direct-to-swapchain stores no longer double-swizzle BGRA swapchains (the storage view already uses the swapchain format), which swapped red and blue and gave a blue tint.
  - The contrast S-curve and saturation boost ("beautify") is now opt-in with `XFSR_EDGE_BEAUTIFY=1`, so by default edge leaves colors unchanged.
- **Freeze on quality/scale change with edge**: `FrameTransfer::resetPipeline` waits on its own slot fences (2 s timeout) instead of `vkDeviceWaitIdle` on the app device.

## [0.7.0] — 2026-10-31

### Added

- **Always-On Dual-GPU iGPU Offload**:
  - Offload to iGPU enabled by default across all titles (`XFSR_OFFLOAD=1`), routing rendering on discrete GPU (dGPU) and post-processing/upscaling/presentation on integrated GPU (iGPU).
  - One-way direct WSI presentation on iGPU supported under Mesa Zink (OpenGL) using monotonic opaque swapchain handles and triple-buffered WSI (`minImageCount >= 3`) to eliminate acquire stalls.
  - Fused asynchronous round-trip offload pipeline (`OffloadRoundTrip::submit` / `submitBlit`) across all spatial upscalers (`EdgePass`, `FsrPass`, `NisPass`, and `Blit`), replacing the synchronous multi-copy CPU blocking fallback.
  - Offload resilience: never falls back to single-GPU or disables offload when render and display extents match (1:1 stubborn titles) or when one-way WSI fails; seamlessly transitions to round-trip iGPU offload.
  - Overlay PCIe optimization: overlay texture buffer upload is gated on text changes, eliminating redundant 460 KB host-to-device transfers every frame.
- **Universal Vulkan and OpenGL Compatibility**:
  - Automatic OpenGL and Zink detection in `scripts/xfsr-run` across both Linux ELF binaries and Windows PE executables (`opengl32.dll`, `glew32.dll`, OpenGL entry points).
  - Seamless Proton / Wine WOW64 support: automatically exports `PROTON_USE_WOW64=1` for 32-bit Windows games so the 64-bit `libVkLayer_XFSR.so` Vulkan layer attaches correctly.
  - Extended OpenGL interceptor (`gl/xfsr_gl_shim.c`) with `glBlitFramebuffer`, `SDL_GL_GetDrawableSize`, and `SDL_GetWindowSizeInPixels` hooks so FBO-blitting engines and SDL games properly scale to the shrunk swapchain.
  - Surface capability shrinking fixes (`applyCapsShrink`): dynamically queries live surface display extents, advertises scaled sizes in `currentExtent` and `maxImageExtent`, and keeps `minImageExtent` un-raised to preserve in-game low-resolution choices (e.g. Resident Evil Village).
- **Beautifying High-Performance Edge Upscaler for Weaker Hardware**:
  - Re-engineered [edge_pass.comp.glsl](shaders/edge_pass.comp.glsl) with **zero shared memory (LDS)** and **zero barriers**, achieving 100% GPU occupancy on weaker discrete cards (GTX 1650, Pascal/Turing), integrated APUs, and handhelds.
  - **Sub-Pixel Edge Tangent Reconstruction**: Traces continuous edge normals and blends along edge tangents to eliminate jagged staircases on geometry silhouettes in low rendering resolutions.
  - **Diagonal-Adaptive Edge Reconstruction**: Smooths 45° and 135° diagonal boundaries without blur or moiré artifacts.
  - **Anti-Ringing Catmull-Rom Texture Filter**: Enforces strict local bounding-box clamps to eradicate dark and bright halos around text, UI elements, foliage, and specular highlights.
  - **Integrated Contrast-Adaptive Sharpening (CAS)**: Recovers crisp micro-detail in textures while suppressing noise on flat gradients.
  - **Clarity & Vibrancy Beautification**: Applies subtle S-curve contrast and selective saturation recovery to lift muddy, washed-out low-resolution frames into vibrant, rich visuals.
  - **Fast Perf Mode (Edge-Lite)**: Ultra-low latency 5-tap diagonal reconstruction with under 0.15 ms execution time for maximum framerates.
  - Defaulted EdgePass to the high-fidelity `quality` mode with default sharpness of `0.60`.
- **In-Game Resolution Indicator**:
  - HUD and status outputs now label sub-1.35x non-standard aspect or internal resolutions as `in-game` (e.g. `in-game 1.11x`) rather than misleadingly bucketing them into the 1.5x `quality` preset label.
- **Native Game Profiles**:
  - Call of Duty 4 (`[app:7940]`, D3D9 `iw3mp.exe` / `iw3sp.exe`): direct layer upscaling with `gamescope=0`, `caps_shrink=1`, `upscaler=edge`, `overlay=1`.
  - Street Fighter 30th Anniversary Collection (`[app:586200]`, OpenGL) & Mega Man Legacy Collection (`[app:363440]`, 32-bit D3D11 `Proteus.exe`): `gamescope=1` with native Wayland backend.
  - Resident Evil Village (`[app:1196590]`): `preset=quality`, `upscaler=edge`, `edge_mode=quality`, `edge_sharpness=0.65`, `rcas=0`, `offload=1`, `gamescope=0`.

### Fixed

- **Adaptive Scaling Stutter Elimination**:
  - Disabled adaptive continuous scaling by default (`adaptive=0`) in [share/xfsr/profiles.example.conf](share/xfsr/profiles.example.conf).
  - Replaced fluctuating adaptive swapchain recreations with rock-solid fixed scaling presets (`quality`, `balanced`, `performance`, `ultra`), eliminating presentation hitches and frame drops.
- **Live Upscaler Switching & On-Screen Feedback**:
  - Added lazy `DeviceContext` initialization for FSR1 (`ps->fsr.init(c)`) in [layer/xfsr_layer.cpp](layer/xfsr_layer.cpp) during live `F3` cycling, preventing live resize failures when switching from Edge to FSR1.
  - Enabled status overlay and toasts by default (`overlay=1`) for Call of Duty 4 (`[app:7940]`) in [share/xfsr/profiles.example.conf](share/xfsr/profiles.example.conf) so preset/mode adjustments are clearly visible on-screen.
  - Restored `r_fullscreen "1"` configuration for Call of Duty 4 to ensure the proxy layer engages full-display upscaling from low in-game resolutions.
- **Presentation Deadlock**: Upgraded `g_queueSubmitMu` to `std::recursive_mutex` in [layer/xfsr_layer.cpp](layer/xfsr_layer.cpp), eliminating a self-deadlock when `pd.QueueSubmit` invokes `xfsr_QueueSubmit` from the present thread.
- **In-Game Resolution Proxy Preservation**: Guarded swapchain recreation on `F1` preset changes with `capsShrinkEnabled()`; titles using in-game resolution scaling (`caps_shrink=0`, e.g. Resident Evil Village) no longer get forced into `VK_ERROR_OUT_OF_DATE_KHR`, preventing accidental fallback to identity passthrough mode.
- **Direct dGPU Dispatch**: Bound `deviceGdpa` directly to the adopted dGPU context in `ensureDgpuAdopted` and `xfsr_CreateSwapchainKHR`, avoiding recursive layer interceptors and optimizing `vkQueueSubmit2` dispatch.

### Changed

- Native layer upscaling for Resident Evil Village (`app:1196590`): enabled direct Vulkan layer proxy with `edge` upscaler, `caps_shrink=0`, and overlay support. Live switching between Edge, FSR1, NIS, and Blit now works seamlessly via hotkeys.
- NVIDIA: do **not** auto-skip gamescope when `--any` / `--gamescope` / profile
  `gamescope=1` is set (Village/CS2 need the nest; layer stays identity). Opt out
  with `XFSR_GAMESCOPE_FORCE=0`. Village profile: `gamescope=1` + `gamescope_fs=1`
  + NIS filter

## [0.6.0] — 2026-10-26

### Added

- Sampler mip LOD bias: `vkCreateSampler` injects scale-aware negative `mipLodBias`
  (`-log2(scale)` by default) so low internal res still samples sharp mips.
  `XFSR_LOD_BIAS=auto|off|<float>`, `XFSR_LOD_BIAS_OFFSET`, `--lod-bias`,
  `--lod-bias-offset`. Skips compare/shadow and single-mip samplers
- EdgePass Di Zenzo: RGB structure tensor (per-channel Sobel outer products) with
  3×3 box-smoothed shared tile, ETF anisotropy-weighted along-edge mix, fused
  single-pass CAS (quality + perf paths)
- `xfsr-run --any` / `--universal` — gamescope nest that forces the game window to
  the preset render size (upscales titles that keep the Vulkan layer on identity).
  NVIDIA hybrids auto-use `--backend sdl` instead of disabling the nest
- `--gamescope-backend` / `XFSR_GAMESCOPE_BACKEND`; profiles for CS2 / CoD4 / Village
  default `gamescope=1`
- Gamescope nest: `ENABLE_GAMESCOPE_WSI=1` on AMD/Deck (XWayland bypass). On
  NVIDIA default **WSI off** (Valve#1182 black/white screen; DXVK uses nested
  X11/xcb instead). Override with `XFSR_GAMESCOPE_WSI=0|1`. Never set
  `PROTON_ENABLE_WAYLAND` on NVIDIA; Wayland nest via `XFSR_GAMESCOPE_WAYLAND=1`
- Nest HUD: do not enable `--mangoapp` on NVIDIA (EGL/GLFW crash-loop); opt-in
  via `XFSR_GAMESCOPE_MANGOAPP=1`
- Nest present on Plasma Wayland: SDL host with `SDL_VIDEODRIVER=x11` (real
  KWin Xwayland window). `xfsr-gamescope-focus` sets ChimeraOS-style
  `GAMESCOPECTRL_BASELAYER_*` so the nest composites the game (desktop Steam
  leaves `FOCUSED_APP_GFX` empty → black). `-e` only with
  `XFSR_GAMESCOPE_STEAM=1`. No default `-f`
- EdgePass quality pack: shared-mem source tile, edge-stopping cubic, anti-ring,
  post-scale CAS, linear/sRGB, scale-aware sharpen; `XFSR_EDGE_MODE=perf` / `--edge-mode
  perf` lite path; live `edge_sharp` / `--edge-sharpness`; F2 toggles edge quality↔perf
  when on `edge`; STORAGE→swapchain direct on non-NVIDIA present (hybrid iGPU)
- EdgePass v2: padded display-space CAS (no 16×16 tile seams), luma-only cubic +
  chroma bilinear, Sobel/structure-tensor along-edge mix, linear anti-ring
- `xfsr_load_vk` / `xfsr_load_gl` default to run until Ctrl+C / Esc (keep `--seconds` for benches)

### Fixed

- `--gamescope-fs` / profile `gamescope_fs=1` / `XFSR_GAMESCOPE_FS=1` — gamescope
  host fullscreen (`-f`) while the game stays at nest `-w/-h` (CoD4 windowed fix)
- Gamescope nest floor 640×480 (CoD4 / D3D9 reject ultra 768×432)
- NVIDIA nest: skip gamescope by default (host stays black); use layer instead;
  `XFSR_GAMESCOPE_FORCE=1` to allow nest anyway
- Proxy acquire: finite `appImgFence` wait + re-signal on timeout (Village froze
  after `first present ok` on UINT64_MAX). Village profile: blit + caps-shrink
- `--gamescope`: no xfsr overlay (layer off) — MangoHud on the game; Zink without
  xfsr shim; match load-app window to nest size; drop nested `--debug-hud` /
  `--force-windows-fullscreen` (caused blink/swapchain thrash); warn that
  `--hybrid`/`--offload` are ignored
- Docs: prefer `xfsr-run --any` for titles that stay identity; NVIDIA nest uses
  SDL backend instead of disabling gamescope

## [0.5.0] — 2026-10-25

### Added

- `--gamescope` / profile `gamescope=1` — nest at preset render size, upscale with
  gamescope `-F` (from `--upscaler` or `--gamescope-filter`). Unblocks CS2/Source 2
  where the Vulkan layer stays identity. Layer off unless `XFSR_GAMESCOPE_LAYER=1`
- `--upscaler edge` / `EdgePass` — heavier edge-aware spatial upscaler (F3: fsr1→blit→nis→edge)
- `--hybrid` / `XFSR_HYBRID=1` — dGPU low-res → iGPU `edge` + **one-way present** (no
  full-res back-haul). Status `mode=hybrid-edge`. If one-way WSI fails, same-GPU
  `edge` fallback (refuses silent round-trip). Skips MangoHud; pins xfsr overlay
- [ROADMAP.md](ROADMAP.md) — adaptive FSR, temporal/NIS, profiles, dma-buf, auto-tune
- Per-game profiles: `~/.config/xfsr/profiles.conf` (from
  `share/xfsr/profiles.example.conf`); match Steam AppID / exe / substring;
  `--no-profile`, `--caps-shrink` / `--no-caps-shrink`
- `scripts/xfsr-auto-tune` — short preset×RCAS matrix on `xfsr_load_vk`; prints a
  profile section; `--apply` writes `~/.config/xfsr/profiles.conf`; scores
  `avg` + `p1_fps` (1% lows) and frame-time stdev
- Adaptive FSR: `--adaptive` / `XFSR_ADAPTIVE=1` + `--target-fps` — continuous float
  scale (not only the preset ladder) with pixel hysteresis before recreate
  (control: `adaptive`, `target_fps`, `scale`)
- `--upscaler fsr1|blit|nis` / `XFSR_UPSCALER` — NIS-style edge-directional spatial
  upscaler (`NisPass`)
- `xfsr-run --check` — short `xfsr_load_vk` smoke; reads
  `$XDG_RUNTIME_DIR/xfsr-status` (`mode=proxy|proxy-nis|proxy-edge|hybrid-edge|identity`)
- Overlay pins by default when MangoHud is not requested
- Bench lines include `p1_fps=` and `ft_stdev_ms=`
- One-way offload skips MangoHud and pins the xfsr overlay with FPS, CPU%, GPU%
  and temperatures (sysfs). Native Vulkan may present on the iGPU; Zink stays on
  round-trip (NVIDIA calls present with the app swapchain — opaque handles SIGSEGV)
- Uniform render size: `window / scale` for Vulkan, DXVK, and Zink (windowed and
  fullscreen). No special-case 1080p table that ignored smaller windows
- Docs: 32-bit Proton titles (CoD4) need `PROTON_USE_WOW64=1` so 64-bit DXVK loads
  the layer; borderless + lower in-game res for D3D9 FSR

### Fixed

- Zink on NVIDIA+AMD hybrids: put NVIDIA first in `VK_DRIVER_FILES` (keep Mesa
  Gallium/Zink — do **not** set `__GLX_VENDOR_LIBRARY_NAME=nvidia`, which bypassed
  Zink and killed the overlay); inject `--ozone-platform=x11` for Chrome when
  Wayland is cleared; warn on browser + Wayland
- NIS + live F4 offload: do not record iGPU `NisPass` into the dGPU present
  command buffer (SIGSEGV); use a blocking transfer→NIS→transfer round-trip
- CoD4 / D3D9: do not return proxy images smaller than CreateSwapchain extent
  (DEVICE_LOST after first present); identity + hint when the game ignores caps
- Caps advertise `currentExtent=UINT_MAX` + `max=render` for DXVK; Zink uses a
  concrete `currentExtent=render` (UINT_MAX made Mesa OOM / stay identity)
- Hook `vkGetPhysicalDeviceSurfaceCapabilities2KHR` (DXVK prefers this)
- FSR proxy + caps shrink on integrated GPUs (`discrete=0`, e.g. Vega)
- Identity / exclusive-fullscreen F10 overlay (CoD4): adopt app device, preserve
  `PRESENT_SRC` frame contents, reset overlay sync on DestroyDevice
- Zink X11: publish `XFSR_GL_REMAP` so the shim remaps viewports when EGL still
  reports the window size (zoomed bottom-left crop)
- Skip futile shrink-kick when D3D9 already ignored the caps clamp

### Changed

- Docs / profiles: hybrid max-FPS path is **same-GPU + blit/nis + ultra/adaptive**
  (not `--offload`); FSR2/3/4 called out as out of layer scope (Proton FSR4 is
  AMD-only). Steam / example launch lines and `profiles.example.conf` defaults
  follow that guidance
- `--fsr 0` / `--blit` starts with scale + linear blit (FSR1 pass off); `--native`
  is passthrough (old `--fsr 0` behavior)
- `--offload` without `--no-one-way` prefers one-way and does not load MangoHud
- `xfsr-run --one-way` / offload docs: round-trip is `--no-one-way`; Zink offload
  is always round-trip on NVIDIA hybrids

## [0.4.0] — 2026-10-25

### Added

- In-game controls (default on): **F1** preset, **F2** RCAS, **F3** FSR,
  **F4** offload, **F5** one-way, **F10** pin overlay; toast on change.
  Disable with `XFSR_HUD=0` / `--no-controls`
- Hotkeys via X11 and/or evdev (`/dev/input`); `XFSR_HUD_MOD=1` requires Shift
- Control file `$XDG_RUNTIME_DIR/xfsr-control` (or `XFSR_CONTROL`): `preset`,
  `rcas`, `fsr`, `offload`, `one-way`, `overlay`, `status`
- **F4 live offload**: same-GPU ↔ round-trip without restart (keeps dGPU WSI)
- `--overlay` / `XFSR_OVERLAY=1` — pin status strip at launch (Proton often eats F10)

### Fixed

- SIGSEGV on rapid swapchain recreate (glmark2 scenes / preset hotkey): drain present
  fences before freeing proxy; keep status overlay on the device, not the swapchain
- Clearer log when exclusive fullscreen locks size (`app == display`) so FSR cannot
  shrink — use borderless windowed
- Live F4 offload: always inject host/dma-buf device extensions on discrete CreateDevice
  so mid-run transfer init does not call null ICD entry points
- Identity-path F10 overlay + fence hang; bench recreates on `OUT_OF_DATE` (F1)
- Ambient `XFSR_IGPU_PRESENT=force` no longer overrides `XFSR_OFFLOAD=0`
- F3 FSR-on after blit / live F4 with FSR off: always allocate FSR outputs so
  `fsr.record` does not SIGSEGV
- Bench `--fullscreen` / `--windowed` for VK and GL loaders
- `xfsr-run` / `xfsr-bench`: prefer X11 for native Vulkan FSR when `DISPLAY` is
  set (Wayland `UINT_MAX` extent skipped shrink → flat FPS vs baseline)
- `xfsr-bench`: full matrix — X11+Wayland × windowed+fullscreen, all presets,
  offload±RCAS, VK+GL (`XFSR_BENCH_QUICK=1` / `XFSR_BENCH_X11_ONLY=1` /
  `XFSR_BENCH_WAYLAND_ONLY=1` to narrow)
- **Wayland Vulkan FSR**: seed window size from the first swapchain (ICD
  `currentExtent` is `UINT_MAX`), kick one `OUT_OF_DATE`, clamp caps max to
  render size so the recreate builds a real proxy (was identity @ full res)
- **Wayland Zink/GL FSR**: shim shrinks `wl_egl_window` to render size (Zink
  ignores Vulkan caps); layer treats matching create as hint-honored proxy
- Offload+MangoHud SIGSEGV on Mesa one-way (opaque WSI): refuse one-way when
  MangoHud is active; skip `--offload` when the app is already on the iGPU
  (`--igpu` / Zink-on-Vega)

## [0.3.1] — 2026-10-24

### Fixed

- `glmark2-wayland` postage-stamp: don’t caps-shrink from `DISPLAY_EXTENT` alone;
  skip GL viewport remap when the EGL drawable is still window-sized (Wayland Zink
  identity path). X11 `glmark2` keeps FSR via caps-shrink.

## [0.3.0] — 2026-10-24

### Fixed

- Zink/glmark2 windowed zoom-crop: GL shim remaps with **window/scale**, not the
  1080p `XFSR_RENDER_*` hint (800×600 was forced to 1280×720 / 768×432 → bottom-left
  blow-up). `glfwGetFramebufferSize` uses the same rule.

## [0.2.4] — 2026-10-24

### Added

- `xfsr-run --one-way` / `--no-one-way` (sets `XFSR_IGPU_PRESENT`; `--one-way` implies
  `--offload`)

### Fixed

- NVIDIA `--one-way`: opaque WSI SIGSEGV on swapchain recreate / MangoHud — ignore
  one-way on NVIDIA unless `XFSR_IGPU_PRESENT=force`
- `glmark2-wayland`: keep `WAYLAND_DISPLAY` for `*-wayland` binaries (was cleared for
  Zink and failed canvas init)
- Zink windowed postage-stamp: don’t treat `DISPLAY_EXTENT` hint as the real window;
  only caps-honor when app extent matches expected render size; tighten GL shim remap;
  caps-shrink for Zink on iGPU too

### Changed

- Document offload knobs (`XFSR_IGPU_PRESENT`, `XFSR_DMABUF`) in README

## [0.2.3] — 2026-10-24

### Added

- Offload present policy: try **one-way iGPU present** on Mesa dGPUs; auto-fallback
  to **round-trip** (dGPU WSI) on create/present failure; `XFSR_IGPU_PRESENT=0|1`
- `FrameTransfer` backends: **dma-buf** → shared-host → memcpy (pipelined for
  dma-buf/host); `XFSR_DMABUF=0|1` override. NVIDIA↔Mesa skips dma-buf by default
  (probes OK then DEVICE_LOSTs on RADV)

### Fixed

- Native Vulkan `DEVICE_LOST` after first present: caps-shrink so apps honor render
  extent (unsafe force-scale alone created FB > image); disable FSR direct-to-WSI
  STORAGE path on NVIDIA
- Zink+`--offload` SIGSEGV with MangoHud: serialize proxy present vs swapchain
  recreate (`g_proxyLife`) before tearing down `FrameTransfer`
- GL postage-stamp / missing HUD under Zink+FSR: remap viewports to render size;
  use Vulkan MangoHud (GL HUD was painted on a surface XFSR never samples)
- `xfsr-bench.sh`: MangoHud on all cases; force `--zink` for GL; build with
  `XFSR_BUILD_BENCH=ON`
- Bench `xfsr_load_vk` Y-flip vs OpenGL (Vulkan NDC)

### Changed

- Caps-shrink enabled for native Vulkan by default (`XFSR_CAPS_SHRINK=0` for Source2)
- NVIDIA hybrids: `--offload` defaults to round-trip (one-way opt-in via
  `XFSR_IGPU_PRESENT=1`)

## [0.2.0] — 2026-10-24

### Added

- OpenGL via Zink FSR (same-GPU and `--offload`) with `libxfsr_gl_shim.so`
- `--preset ultra` (2.5× / 768×432 @ 1080p) for weak GPUs
- Async Zink `--offload` round-trip (`transferPipeline` both ways)

### Fixed

- GL viewport corner crop: export shim symbols, wrap `glX`/`egl`/`glfw` GetProcAddress, interpose `dlsym` (glmark2)
- Zink + NVIDIA `--offload` SIGSEGV (keep dGPU WSI; FSR on iGPU via host round-trip)
- CI release: use Ubuntu 24.04 so `glslc` is available
- Steam Vulkan/Proton titles (CS2, DXVK): Zink no longer on by default (was clearing
  `WAYLAND_DISPLAY` and forcing `GALLIUM_DRIVER=zink`)
- CS2 stuck on logo: stop caps-shrinking native Vulkan (recreate storm); skip
  `QueueWaitIdle` on swapchain destroy; don’t inherit ambient `XFSR_OUTPUT_*`

### Changed

- Gate `[xfsr] presented N frames` on `XFSR_VERBOSE`
- Layer prefers `XFSR_SCALE` from `xfsr-run` when set
- `xfsr-run`: `--zink` is opt-in; Vulkan/Proton use the layer with no Zink env

## [0.1.0] — 2026-10-24

### Added

- Project renamed to **xfsr** (layer `VK_LAYER_XFSR`, env `XFSR_*`; legacy `CROSSVULKAN_*` still accepted)
- `xfsr-run` Vulkan layer installer and Steam-friendly launcher
- Same-GPU FSR1 upscale (default) and optional `--offload` dual-GPU present
- Presets: quality / balanced / performance
- `--no-rcas` (EASU-only) for higher FPS
- Wayland display-extent hint (`--output` / `XFSR_DISPLAY_EXTENT`)
- `xfsr-bench.sh` / `xfsr_load_vk` / `xfsr_load_gl` uncapped load tools
- One-line install script and GitHub Actions release packages

### Fixed

- Postage-stamp on Wayland from invented `app × scale` display sizes
- Dual-GPU `SURFACE_LOST` / early `OUT_OF_DATE` (opaque app handle + iGPU-only window WSI)
- Caps-shrink vs Zink EGL drawable mismatch

[Unreleased]: https://github.com/jersobh/edge/compare/v0.10.0...HEAD
[0.10.0]: https://github.com/jersobh/edge/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/jersobh/edge/compare/v0.8.3...v0.9.0
[0.8.3]: https://github.com/jersobh/edge/compare/v0.8.2...v0.8.3
[0.8.2]: https://github.com/jersobh/edge/compare/v0.8.1...v0.8.2
[0.8.1]: https://github.com/jersobh/edge/releases/tag/v0.8.1
[0.8.0]: https://github.com/jersobh/edge/releases/tag/v0.8.0
[0.7.0]: https://github.com/jersobh/edge/releases/tag/v0.7.0
[0.6.0]: https://github.com/jersobh/edge/releases/tag/v0.6.0
[0.5.0]: https://github.com/jersobh/edge/releases/tag/v0.5.0
[0.4.0]: https://github.com/jersobh/edge/releases/tag/v0.4.0
[0.3.1]: https://github.com/jersobh/edge/releases/tag/v0.3.1
[0.3.0]: https://github.com/jersobh/edge/releases/tag/v0.3.0
[0.2.4]: https://github.com/jersobh/edge/releases/tag/v0.2.4
[0.2.3]: https://github.com/jersobh/edge/releases/tag/v0.2.3
[0.2.0]: https://github.com/jersobh/edge/releases/tag/v0.2.0
[0.1.0]: https://github.com/jersobh/edge/releases/tag/v0.1.0
