<p align="center">
  <img src="logo.png" alt="EdgeScale Logo" width="180" />
</p>

<h1 align="center">EdgeScale</h1>

<p align="center">
  <a href="https://github.com/jersobh/edge/releases/latest"><img src="https://img.shields.io/github/v/release/jersobh/edge?display_name=tag&sort=semver" alt="Release" /></a>
  <a href="https://github.com/jersobh/edge/actions/workflows/release.yml"><img src="https://github.com/jersobh/edge/actions/workflows/release.yml/badge.svg" alt="Build" /></a>
  <a href="https://github.com/jersobh/edge/releases"><img src="https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20x86__64-lightgrey.svg" alt="Platform" /></a>
</p>

**EdgeScale (`edgerun`)** — high-performance in-process GPU upscaling and companion iGPU offload for Vulkan games (Linux native, Proton/Wine, OpenGL via Zink, and Windows native Vulkan / DXVK).

Wrap your game launch in Steam with `edgerun %command%` to render lower and reconstruct crisp native resolution frames with near-zero latency.

**Current version:** [0.8.2](https://github.com/jersobh/edge/releases/tag/v0.8.2)  
*(Formerly known as `xfsr`; `xfsr-run` is preserved as an alias for full backward compatibility).*

**Documentation & Deep-Dive:**
- [Architecture & Deep Comparison with Lossless Scaling & Magpie](docs/ARCHITECTURE_AND_COMPARISON.md)
- [Project Roadmap](ROADMAP.md) — adaptive scaling, profiles, auto-tune, dma-buf.

## What it does

Games spend most of their time rasterizing each frame. **EdgeScale** asks the game to draw at a lower internal resolution (e.g. 960×540 or 1280×720 instead of 1920×1080), then reconstructs the image to your display using:
- **`edge` (Default / Recommended):** High-occupancy sub-pixel edge tangent reconstruction, anti-ringing Catmull-Rom cubic filtering, and Contrast-Adaptive Sharpening (CAS).
- **`fsr1`:** AMD FidelityFX Super Resolution 1.0 (EASU + optional RCAS).
- **`nis`:** NVIDIA Image Scaling compute shader.
- **`blit`:** Pure linear blit at preset scale.

You do **not** need the game to have built-in upscaling. EdgeScale hooks in through an in-process **Vulkan layer**: it sits directly inside the game's swapchain and intercepts frames with zero capture lag.

**Works with:** Native Vulkan, Proton/DXVK (64-bit and 32-bit via automatic `PROTON_USE_WOW64=1`), and OpenGL via automatic Zink detection (with an `LD_PRELOAD` shim that remaps viewports, scissors, FBO blits, and SDL drawable queries).  
**Modes:** Dual-GPU iGPU offload by default (`--offload` with one-way iGPU presentation or fused round-trip fallback), `--hybrid` (dGPU render + iGPU `edge` upscale + one-way present), or `--no-offload` (same-GPU).  
**Tested on:** Hybrid laptops (e.g. NVIDIA dGPU + AMD Vega iGPU) and discrete cards.

### Quick Steam Launch Options

```text
edgerun --preset performance %command%
edgerun --hybrid --preset performance %command%
edgerun --any --preset performance --gamescope-fs %command%
```
*(You can also continue using `xfsr-run` interchangeably).*

### Max FPS on hybrid NVIDIA + AMD (e.g. GTX 1650 + Vega)

1. **Same-GPU first** for peak FPS — round-trip `--offload` often halves bench FPS.
2. **Cheapest upscaler first:** `blit` → `nis` → `edge` → `fsr1 --no-rcas`.
3. **Use the iGPU usefully with `--hybrid`:** dGPU renders low-res only; Vega runs the `edge` upscaler and presents (no full-res back-haul). Confirm `mode=hybrid-edge`. If one-way WSI fails, falls back to same-GPU `edge` (never silent round-trip).
4. **Harder preset when GPU-bound:** `ultra` / `performance`, or `--adaptive --target-fps 60`.
5. **Confirm engagement:** `xfsr-run --layer-dir ./build --check` → `mode=proxy*` / `hybrid-edge` and NVIDIA discrete for the game.

```text
xfsr-run --mangohud --upscaler blit --preset ultra --no-rcas --overlay %command%
xfsr-run --hybrid --preset ultra --overlay %command%
xfsr-run --adaptive --target-fps 60 --upscaler nis --no-rcas --overlay %command%
xfsr-run --zink --upscaler blit --preset ultra --no-rcas glmark2
```

**Any title (CS2 / Village / DXVK that stay identity):** nest in gamescope (forces the
game window to the preset render size; gamescope upscales to your display):

```text
xfsr-run --any --preset quality --upscaler edge --gamescope-fs %command%
```

On **NVIDIA** the host uses SDL+X11 and WSI off. If the nest stays black, set
`XFSR_GAMESCOPE_FORCE=0` (layer-only) or a low Borderless res for titles that honor it.

**Vulkan layer** only upscales when the game creates a smaller swapchain (`proxy ready`). If you see `identity` at desktop size, use `--any` (gamescope nest).

Avoid for FPS: plain `--offload` round-trip, `XFSR_IGPU_PRESENT=force` with MangoHud, Chrome + Wayland as an FSR test.

### Modes

| Mode | How | When to use |
|------|-----|-------------|
| **Dual-GPU Offload** (default) | dGPU renders low-res → transfer → iGPU upscales + **one-way present** (or fused async round-trip fallback) | Free dGPU for the game while iGPU handles upscaling and display presentation |
| **`--hybrid`** | dGPU low-res → transfer → iGPU `edge` upscale + **one-way present** | High-quality directional EdgePass upscale on iGPU |
| **Same-GPU (`--no-offload`)** | Game renders low-res on the GPU it already uses; that same GPU upscales and presents | Single-GPU systems or when PCIe transfer overhead exceeds upscale cost |

Presets control how hard it pushes:

| Preset | Scale (approx.) | Example at 1080p |
|--------|-----------------|------------------|
| `quality` | 1.5× | 1280×720 → 1920×1080 |
| `balanced` | 1.7× | ~1129×635 → 1920×1080 |
| `performance` | 2.0× | 960×540 → 1920×1080 |
| `ultra` | 2.5× | 768×432 → 1920×1080 |

`--no-rcas` skips FSR’s sharpening pass (EASU upscale only) — often a bit more FPS, slightly softer image.

## Install

### Linux (One-Line Installer)

```sh
curl -fsSL https://raw.githubusercontent.com/jersobh/edge/main/install.sh | sh
```

Installs into `~/.local` (`edgerun`, `xfsr-run` alias, Vulkan layer, shaders). Put `~/.local/bin` on your `PATH` if it isn’t already.

### Windows (Native Vulkan Layer)

1. Download the latest `edgescale-windows-x86_64.zip` from [Releases](https://github.com/jersobh/edge/releases/latest).
2. Extract the archive and double-click `install-windows.bat`.  
   *(Installs to `%LOCALAPPDATA%\EdgeScale` and registers `VkLayer_EdgeScale.json` in `HKCU\Software\Khronos\Vulkan\ImplicitLayers`)*.
3. Launch your game or Steam with environment variable:
   ```cmd
   set ENABLE_EDGESCALE=1
   ```
   Or in Steam Game Launch Options (Windows):
   ```cmd
   cmd /c "set ENABLE_EDGESCALE=1 && %command%"
   ```
4. Control presets and modes live in-game using hotkeys:
   - **F1**: Cycle preset (`quality` &rarr; `balanced` &rarr; `performance` &rarr; `ultra`)
   - **F2**: Toggle RCAS / Edge quality vs perf mode
   - **F3**: Cycle upscaler (`edge` &rarr; `fsr1` &rarr; `nis` &rarr; `blit`)
   - **F10**: Toggle HUD overlay pinning

## Steam

**Any Steam / Proton / DXVK / Source 2 title** (recommended; needed for Village):

```text
edgerun --any --preset quality --upscaler edge --gamescope-fs %command%
```

**Layer-only** (when `edgerun --check` shows `proxy*` and the game honors shrink):

```text
edgerun --overlay --upscaler edge --preset performance %command%
```

Do **not** add `--zink` for native Vulkan.

You should see `edgerun: layer …` then **`[xfsr] layer active:`** (and later `force-scale` / `proxy ready` / `hotkeys:`) in the Steam console. If Mega Man / Proton never prints `[xfsr]`, the layer `.so` failed to load inside steamrt — reinstall from this tree (`./scripts/install-edgerun.sh`; the layer is linked with a pressure-vessel host-lib RPATH), fully quit Steam, clear leftover env, relaunch Steam, and retry.

**32-bit Windows titles** (CoD4:MW, many 2000s D3D9/OpenGL games): the layer is **x86_64**. `edgerun` automatically exports `PROTON_USE_WOW64=1` for Proton launches so 32-bit Windows games run under 64-bit Wine Wow64 and load `libVkLayer_XFSR.so`:

```text
gamemoderun xfsr-run --preset performance --overlay --no-rcas %command%
```

Confirm with `hotkeys:` or `identity overlay armed` / `proxy ready` in the Steam console. Prefer **borderless windowed** so FSR can shrink (exclusive fullscreen often locks `app == display`).

Clear leftover test env before starting Steam (empty `VK_ICD_FILENAMES=` or stale `XFSR_OUTPUT_*=800x600` will break games):

```fish
set -e GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE ENABLE_XFSR
set -e XFSR_OUTPUT_W XFSR_OUTPUT_H XFSR_DISPLAY_EXTENT XFSR_OFFLOAD XFSR_VERBOSE
set -e VK_ADD_IMPLICIT_LAYER_PATH VK_LAYER_PATH VK_IMPLICIT_LAYER_PATH VK_ICD_FILENAMES
```

Confirm the layer is live: `xfsr-run --check`, or `XFSR_VERBOSE=1` (look for `proxy ready` / `force-scale` / `first present ok`). Auto-tune a preset: `./scripts/xfsr-auto-tune --apply`.

**OpenGL titles** (glmark2, native GL, and Windows OpenGL games) — automatically detected (`auto` Zink mode) or forced with `--zink`:

```text
xfsr-run --upscaler blit --preset ultra --no-rcas %command%
```

## Usage

```sh
xfsr-run --help
xfsr-run --upscaler blit --preset ultra --no-rcas --overlay vkcube
xfsr-run --upscaler blit --preset ultra --no-rcas glmark2
xfsr-run --layer-dir ./build --check
```

| Flag | What it does |
|------|----------------|
| `--preset quality\|balanced\|performance\|ultra` | How much to lower render resolution (`ultra` = 2.5×) |
| `--fsr 1` | FSR1 upscale (default) |
| `--fsr 0` / `--blit` | Same scale, linear blit instead of FSR1 (launch as F3 off) |
| `--upscaler fsr1\|blit\|nis\|edge` | Spatial upscaler (`edge` = heavier; hybrid default) |
| `--edge-mode quality\|perf` | EdgePass quality (sub-pixel edge tangent + CAS) or perf/lite |
| `--edge-sharpness 0..1` | EdgePass post-scale sharpen (default 0.60) |
| `--lod-bias auto\|off\|<float>` | Negative mip LOD on app samplers (default `auto` = `-log2(scale)`) |
| `--lod-bias-offset <float>` | Added to LOD bias (e.g. `-0.5`) |
| `--hybrid` | Offload + one-way + `edge`; fused round-trip fallback if one-way fails |
| `--gamescope` | Nest game at preset render size; gamescope upscales to `--output` (CS2) |
| `--gamescope-filter fsr\|nis\|linear\|…` | Override gamescope `-F` (default from `--upscaler`) |
| `--native` | No upscale — passthrough swapchain |
| `--no-rcas` | Skip sharpening — usually more FPS |
| `--offload` / `--no-offload` | dGPU render, iGPU upscale + present (default: `--offload`) |
| `--one-way` / `--no-one-way` | iGPU present (default with `--offload`) or fused round-trip. One-way skips MangoHud and pins the xfsr overlay |
| `--mangohud` | FPS overlay. Not loaded on one-way; use the xfsr overlay instead |
| `--controls` / `--no-controls` | In-game hotkeys + status toast (default on) |
| `--overlay` | Pin the status strip at launch (`XFSR_OVERLAY=1`; Proton/Steam often eat F10) |
| `--zink` / `--no-zink` | OpenGL via Zink + viewport/FBO/SDL shim (default: `auto` for OpenGL ELF & `.exe` binaries) |
| `--print` | Show the env xfsr would set, then exit |
| `--no-profile` | Ignore `~/.config/xfsr/profiles.conf` |
| `--caps-shrink` / `--no-caps-shrink` | Allow/forbid smaller `maxImageExtent` (CS2 often needs off) |
| `--adaptive` / `--target-fps N` | Continuous render scale toward a frame-time target |
| `--check` | Smoke-test: confirm proxy FSR vs identity after a short bench |

### Per-game profiles

On first run, `xfsr-run` copies [share/xfsr/profiles.example.conf](share/xfsr/profiles.example.conf)
to `~/.config/xfsr/profiles.conf`. Matching uses `SteamAppId`, then executable name, then a
command-line substring. CLI flags always override the profile. See [ROADMAP.md](ROADMAP.md) §3.

Auto-pick a preset for the built-in Vulkan load app (or your `SteamAppId`):

```sh
./scripts/xfsr-auto-tune --seconds 5          # recommend
./scripts/xfsr-auto-tune --apply              # write ~/.config/xfsr/profiles.conf
SteamAppId=730 ./scripts/xfsr-auto-tune --apply
```

Adaptive (continuous scale toward a target FPS) and a smoke check:

```sh
xfsr-run --adaptive --target-fps 60 --upscaler blit --no-rcas --overlay %command%
xfsr-run --layer-dir ./build --check
```

### In-game controls (default on)

| Key | Action |
|-----|--------|
| **F1** | Cycle preset (**live** via swapchain recreate when the title handles `OUT_OF_DATE`) |
| **F2** | Toggle RCAS (**live**); on `edge`, toggles quality ↔ perf |
| **F3** | Cycle upscaler FSR1 → blit → NIS → edge (**live** when passes exist) |
| **F4** | Toggle offload same-GPU ↔ round-trip (**live**; brief hitch) |
| **F5** | Toggle one-way preference (**restart** to move WSI) |
| **F10** | Pin / unpin the status overlay (**live**) |

A toast appears for a few seconds after each change (and stays if pinned). Keys are read via X11 and/or `/dev/input` (evdev). Set `XFSR_HUD_MOD=1` if you want **Shift+F*** to avoid clashing with game binds. For Proton titles where F10 does nothing, launch with `--overlay` or:

```sh
echo overlay 1 > "$XDG_RUNTIME_DIR/xfsr-control"
```

If hotkeys still do nothing, check the console for `hotkeys: x11=… evdev=…` and use the control file:

```sh
echo preset ultra > "$XDG_RUNTIME_DIR/xfsr-control"
echo rcas 0 >> "$XDG_RUNTIME_DIR/xfsr-control"
```

Commands: `preset …`, `cycle_preset`, `rcas 0|1`, `edge_sharp 0..1`, `edge_mode quality|perf`,
`fsr 0|1`, `upscaler …`, `offload 0|1`, `one-way 0|1`, `overlay 0|1`, `status`.
`offload` applies live (round-trip); `one-way` needs a restart.

**Exclusive fullscreen / D3D9 / CS2:** Many titles create a full-desktop swapchain and
ignore caps shrink → **identity** (looks unchanged; no upscale). For a real proxy:
use **borderless windowed** and set an **in-game resolution below the desktop**
(e.g. 1280×720 on 1080p). Look for `proxy ready: render=… display=…` or
`mode=proxy-edge` / `hybrid-edge` — not `identity` / “wanted render … identity”.
CS2 profile keeps `caps_shrink=0`; drop `-fullscreen` from launch options.
Opt out of caps clamping with `XFSR_CAPS_SHRINK=0` when needed.

Offload knobs (env; same as the flags above):

| Env | Effect |
|-----|--------|
| `XFSR_HUD=0` | Disable in-game controls / overlay |
| `XFSR_OVERLAY=1` | Pin status strip at launch (same as `--overlay`) |
| `XFSR_HUD_MOD=1` | Require Shift+F* for hotkeys (default: bare F-keys) |
| `XFSR_CONTROL=/path` | Override control-file path (default `$XDG_RUNTIME_DIR/xfsr-control`) |
| `XFSR_IGPU_PRESENT=0` | Force round-trip (dGPU WSI) — same as `--no-one-way` |
| `XFSR_IGPU_PRESENT=1` | One-way iGPU present (any vendor). Skips MangoHud |
| `XFSR_IGPU_PRESENT=force` | One-way even if MangoHud is still loaded (often SIGSEGV) |
| `XFSR_DMABUF=1` | Force dma-buf transfer try (default skips NVIDIA↔Mesa) |
| `XFSR_DMABUF=0` | Never use dma-buf |
| `XFSR_VERBOSE=1` | Log `offload: …`, `transfer backend=…`, `first present ok` |

### OpenGL / Zink

`xfsr-run` automatically detects OpenGL Linux ELF and Windows PE (`.exe`) binaries and enables Mesa Zink alongside `libxfsr_gl_shim.so` (which remaps `glViewport`, `glScissor`, `glBlitFramebuffer`, and SDL drawable queries). You can also force Zink explicitly with **`--zink`** or disable it with `--no-zink`. Vulkan, DXVK, and Zink share one scale: render = window / preset, present = window, windowed or fullscreen. Both native Vulkan and Zink support one-way iGPU presentation (`--offload`, using monotonic opaque swapchain handles) as well as fused asynchronous round-trip offload (`--no-one-way`).

**Wayland:** Vulkan FSR works (shrink-kick). OpenGL/Zink FSR works too — the GL shim shrinks `wl_egl_window` to render size so Zink asks for a shrunk swapchain. `xfsr-run --zink` still prefers X11 when both are available (often faster on hybrid NVIDIA+AMD); keep pure Wayland with `XFSR_ZINK_WAYLAND=1`. Native VK: `XFSR_VK_WAYLAND=1`.

## Build from source

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DXFSR_BUILD_DEMO=OFF -DXFSR_BUILD_BENCH=OFF
cmake --build build -j --target VkLayer_XFSR
./scripts/install-xfsr-run.sh          # → ~/.local
./scripts/package-xfsr.sh              # → dist/*.tar.gz
```

Needs CMake ≥ 3.20, C++17, `glslc`, Vulkan loader. FSR1 headers: `third_party/ffx-fsr/`.

## Versioning

- [`VERSION`](VERSION) — **0.8.1**
- [`CHANGELOG.md`](CHANGELOG.md)
- Tag `vX.Y.Z` → GitHub Actions builds the Linux package the install script downloads

```sh
git tag v0.8.1 && git push origin v0.8.1
```

## Credits

FSR 1.0 headers from AMD FidelityFX (`third_party/ffx-fsr/`).
