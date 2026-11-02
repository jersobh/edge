<p align="center">
  <img src="logo.png" alt="EdgeScale Logo" width="180" />
</p>

<h1 align="center">EdgeScale</h1>

<p align="center">
  <a href="https://github.com/jersobh/edge/releases/latest"><img src="https://img.shields.io/github/v/release/jersobh/edge?display_name=tag&sort=semver" alt="Release" /></a>
  <a href="https://github.com/jersobh/edge/actions/workflows/release.yml"><img src="https://github.com/jersobh/edge/actions/workflows/release.yml/badge.svg" alt="Build" /></a>
  <a href="https://github.com/jersobh/edge/releases"><img src="https://img.shields.io/badge/Linux-x86__64-FCC624?logo=linux&logoColor=black" alt="Linux" /></a>
  <a href="https://github.com/jersobh/edge/releases"><img src="https://img.shields.io/badge/Windows-x86__64-0078D6?logo=windows&logoColor=white" alt="Windows" /></a>
</p>

**EdgeScale (`edgerun`)** — high-performance in-process GPU upscaling and companion iGPU offload for Vulkan games (Linux native, Proton/Wine, OpenGL via Zink, and Windows native Vulkan / DXVK).

Wrap your game launch in Steam with `edgerun %command%` to render lower and reconstruct crisp native (or 4K) resolution frames with near-zero latency.

<p align="center">
  <a href="https://youtu.be/JAZmOpItF34">
    <img src="https://img.youtube.com/vi/JAZmOpItF34/maxresdefault.jpg" alt="EdgeScale demo video" width="720" />
  </a>
</p>
<p align="center"><a href="https://youtu.be/JAZmOpItF34">Watch the demo on YouTube</a></p>

**Current version:** [0.8.3](https://github.com/jersobh/edge/releases/tag/v0.8.3)  
*(Formerly known as `xfsr`; `xfsr-run` and `XFSR_*` env vars remain as aliases.)*

**Documentation & Deep-Dive:**
- [Architecture & Deep Comparison with Lossless Scaling & Magpie](docs/ARCHITECTURE_AND_COMPARISON.md)
- [Project Roadmap](ROADMAP.md) — adaptive scaling, profiles, auto-tune, dma-buf.

## What it does

Games spend most of their time rasterizing each frame. **EdgeScale** asks the game to draw at a lower internal resolution (e.g. 960×540 or 1280×720 instead of 1920×1080 / 3840×2160), then reconstructs the image to your display using:
- **`edge` (Default / Recommended):** High-occupancy sub-pixel edge tangent reconstruction, anti-ringing Catmull-Rom cubic filtering, and Contrast-Adaptive Sharpening (CAS).
- **`nis`:** NVIDIA Image Scaling compute shader.
- **`blit`:** Pure linear blit at preset scale.
- **`nearest`:** Nearest-neighbor blit — crisp integer-friendly upscale for **2D pixel art**.

You do **not** need the game to have built-in upscaling. EdgeScale hooks in through an in-process **Vulkan layer**: it sits directly inside the game's swapchain and intercepts frames with zero capture lag.

**Works with:** Native Vulkan, Proton/DXVK (64-bit and 32-bit via automatic `PROTON_USE_WOW64=1`), and OpenGL via automatic Zink detection (with an `LD_PRELOAD` shim that remaps viewports, scissors, FBO blits, and SDL drawable queries).  
**Modes:** Dual-GPU iGPU offload by default (`--offload` with one-way iGPU presentation or fused async round-trip fallback), `--hybrid` (dGPU render + iGPU `edge` upscale + one-way present), or `--no-offload` (same-GPU).  
**Tested on:** Hybrid laptops (e.g. NVIDIA dGPU + AMD Vega iGPU) and discrete cards.

### Quick Steam Launch Options

```text
edgerun --preset performance %command%
edgerun --hybrid --preset performance %command%
edgerun --any --preset performance --gamescope-fs %command%
edgerun --output 3840x2160 --render 1280x720 --upscaler nearest %command%
```
*(You can also continue using `xfsr-run` interchangeably.)*

### Getting the most out of hybrid NVIDIA + AMD

1. **Prefer default `--offload` (one-way)** — dGPU renders low-res; iGPU upscales and presents. Fused async round-trip is only the fallback when one-way WSI is unavailable.
2. **`--hybrid`** for EdgePass on the iGPU (no full-res back-haul). Confirm `mode=hybrid-edge`.
3. **Cheapest upscaler when GPU-bound:** `blit` / `nearest` → `nis` → `edge`.
4. **Harder preset or manual res:** `ultra` / `performance`, `--output 3840x2160`, or `--adaptive --target-fps 60`.
5. **Confirm engagement:** `edgerun --check` → `mode=proxy*` / `hybrid-edge`.

```text
edgerun --hybrid --preset ultra --overlay %command%
edgerun --adaptive --target-fps 60 --upscaler nis --no-rcas --overlay %command%
edgerun --zink --upscaler blit --preset ultra --no-rcas glmark2
edgerun --upscaler nearest --render 640x360 --output 2560x1440 %command%
```

**Any title (CS2 / Village / DXVK that stay identity):** nest in gamescope (forces the
game window to the preset render size; gamescope upscales to your display):

```text
edgerun --any --preset quality --upscaler edge --gamescope-fs %command%
```

On **NVIDIA** the host uses SDL+X11 and WSI off. If the nest stays black, set
`EDGESCALE_GAMESCOPE_FORCE=0` (or legacy `XFSR_GAMESCOPE_FORCE=0`) for layer-only, or a low Borderless res for titles that honor it.

**Vulkan layer** only upscales when the game creates a smaller swapchain (`proxy ready`). If you see `identity` at desktop size, use `--any` (gamescope nest).

Avoid: `EDGESCALE_IGPU_PRESENT=force` with MangoHud (often SIGSEGV).

### Modes

| Mode | How | When to use |
|------|-----|-------------|
| **Dual-GPU Offload** (default) | dGPU renders low-res → transfer → iGPU upscales + **one-way present** (fused async round-trip fallback) | Free dGPU for the game while iGPU handles upscaling and display presentation |
| **`--hybrid`** | dGPU low-res → transfer → iGPU `edge` upscale + **one-way present** | High-quality directional EdgePass upscale on iGPU |
| **Same-GPU (`--no-offload`)** | Game renders low-res on the GPU it already uses; that same GPU upscales and presents | Single-GPU systems |

Presets control how hard it pushes (output auto-detected from your display when `--output` is omitted):

| Preset | Scale (approx.) | Example at 1080p | Example at 4K |
|--------|-----------------|------------------|---------------|
| `quality` | 1.5× | 1280×720 → 1920×1080 | 2560×1440 → 3840×2160 |
| `balanced` | 1.7× | ~1129×635 → 1920×1080 | ~2259×1271 → 3840×2160 |
| `performance` | 2.0× | 960×540 → 1920×1080 | 1920×1080 → 3840×2160 |
| `ultra` | 2.5× | 768×432 → 1920×1080 | 1536×864 → 3840×2160 |

Manual sizes override presets:

```text
edgerun --output 3840x2160 --render 1280x720 --upscaler edge %command%
```

`--no-rcas` disables the optional sharpening pass on upscalers that support it — often a bit more FPS, slightly softer image.

## Install

### Linux (AppImage — recommended)

1. Download `EdgeScale-x86_64.AppImage` from [Releases](https://github.com/jersobh/edge/releases/latest).
2. Make it executable and run it like `edgerun`:

```sh
chmod +x EdgeScale-x86_64.AppImage
./EdgeScale-x86_64.AppImage --preset performance %command%
```

Optional: symlink onto your `PATH`:

```sh
mkdir -p ~/.local/bin
ln -sf "$(pwd)/EdgeScale-x86_64.AppImage" ~/.local/bin/edgerun
```

A versioned `.tar.gz` is also published for manual installs. Legacy one-liner (installs into `~/.local`):

```sh
curl -fsSL https://raw.githubusercontent.com/jersobh/edge/main/install.sh | sh
```

### Windows (Setup.exe — recommended)

1. Download `EdgeScale-*-Setup.exe` from [Releases](https://github.com/jersobh/edge/releases/latest).
2. Run the installer (per-user → `%LOCALAPPDATA%\EdgeScale`, registers the Vulkan implicit layer).
3. Uninstall via **Settings → Apps** (or the Start Menu uninstaller).

Fallback: extract `edgescale-windows-x86_64.zip` and run `install-windows.bat` / `uninstall-windows.bat`.

#### Enabling EdgeScale
- **In Steam (Windows):** Right-click game → **Properties** → **Launch Options**:
  ```cmd
  cmd /c "set ENABLE_EDGESCALE=1 && %command%"
  ```
- **From Command Prompt (CMD):**
  ```cmd
  set ENABLE_EDGESCALE=1
  game.exe
  ```
- **From PowerShell:**
  ```powershell
  $env:ENABLE_EDGESCALE=1
  .\game.exe
  ```

#### In-Game Hotkeys
Control upscaling in real time without relaunching:
- **F1**: Cycle scaling preset (`quality` → `balanced` → `performance` → `ultra`)
- **F2**: Toggle sharpening or Edge quality/perf mode
- **F3**: Cycle upscaler (`blit` → `nearest` → `nis` → `edge`)
- **F10**: Toggle HUD overlay pinning

#### Compatibility on Windows
- **Native Vulkan Games:** DOOM Eternal, Baldur's Gate 3 (Vulkan), No Man's Sky, Red Dead Redemption 2 (Vulkan mode), Rainbow Six Siege (Vulkan), etc.
- **DirectX Games via DXVK:** Drop DXVK (`dxgi.dll`, `d3d11.dll`, or `d3d9.dll`) into the game folder; EdgeScale intercepts the resulting Vulkan swapchain with near-zero latency.

## Steam

**Any Steam / Proton / DXVK / Source 2 title** (recommended; needed for Village):

```text
edgerun --any --preset quality --upscaler edge --gamescope-fs %command%
```

**Layer-only** (when `edgerun --check` shows `proxy*` and the game honors shrink):

```text
edgerun --overlay --upscaler edge --preset performance %command%
```

**4K / pixel art:**

```text
edgerun --output 3840x2160 --preset performance --upscaler edge %command%
edgerun --output 3840x2160 --render 960x540 --upscaler nearest %command%
```

Do **not** add `--zink` for native Vulkan.

You should see `edgerun: layer …` then **`[xfsr] layer active:`** (and later `force-scale` / `proxy ready` / `hotkeys:`) in the Steam console. If Mega Man / Proton never prints `[xfsr]`, the layer `.so` failed to load inside steamrt — reinstall (`./scripts/install-edge-run.sh`; the layer is linked with a pressure-vessel host-lib RPATH), fully quit Steam, clear leftover env, relaunch Steam, and retry.

**32-bit Windows titles** (CoD4:MW, many 2000s D3D9/OpenGL games): the layer is **x86_64**. `edgerun` automatically exports `PROTON_USE_WOW64=1` for Proton launches so 32-bit Windows games run under 64-bit Wine Wow64 and load `libVkLayer_XFSR.so`:

```text
gamemoderun edgerun --preset performance --overlay --no-rcas %command%
```

Confirm with `hotkeys:` or `identity overlay armed` / `proxy ready` in the Steam console. Prefer **borderless windowed** so EdgeScale can shrink (exclusive fullscreen often locks `app == display`).

Clear leftover test env before starting Steam (empty `VK_ICD_FILENAMES=` or stale `EDGESCALE_OUTPUT_*=800x600` / `XFSR_OUTPUT_*=800x600` will break games):

```fish
set -e GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE ENABLE_XFSR ENABLE_EDGESCALE
set -e XFSR_OUTPUT_W XFSR_OUTPUT_H XFSR_DISPLAY_EXTENT XFSR_OFFLOAD XFSR_VERBOSE
set -e EDGESCALE_OUTPUT_W EDGESCALE_OUTPUT_H EDGESCALE_DISPLAY_EXTENT EDGESCALE_OFFLOAD
set -e VK_ADD_IMPLICIT_LAYER_PATH VK_LAYER_PATH VK_IMPLICIT_LAYER_PATH VK_ICD_FILENAMES
```

Confirm the layer is live: `edgerun --check`, or `EDGESCALE_VERBOSE=1` (look for `proxy ready` / `force-scale` / `first present ok`). Auto-tune a preset: `./scripts/xfsr-auto-tune --apply`.

**OpenGL titles** (glmark2, native GL, and Windows OpenGL games) — automatically detected (`auto` Zink mode) or forced with `--zink`:

```text
edgerun --upscaler blit --preset ultra --no-rcas %command%
```

## Usage

```sh
edgerun --help
edgerun --upscaler blit --preset ultra --no-rcas --overlay vkcube
edgerun --upscaler nearest --render 640x360 --output 1920x1080 vkcube
edgerun --layer-dir ./build --check
```

| Flag | What it does |
|------|----------------|
| `--preset quality\|balanced\|performance\|ultra` | How much to lower render resolution (`ultra` = 2.5×) |
| `--output WxH` | Present / display size (default: auto-detect primary display) |
| `--render WxH` | Absolute internal render size (overrides preset-derived scale) |
| `--blit` | Same scale, linear blit (no spatial upscaler) |
| `--upscaler blit\|nearest\|nis\|edge` | Spatial upscaler (`nearest` = pixel art; `edge` = recommended) |
| `--edge-mode quality\|perf` | EdgePass quality (sub-pixel edge tangent + CAS) or perf/lite |
| `--edge-sharpness 0..1` | EdgePass post-scale sharpen (default 0.60) |
| `--lod-bias auto\|off\|<float>` | Negative mip LOD on app samplers (default `auto` = `-log2(scale)`) |
| `--lod-bias-offset <float>` | Added to LOD bias (e.g. `-0.5`) |
| `--hybrid` | Offload + one-way + `edge`; fused round-trip fallback if one-way fails |
| `--gamescope` | Nest game at preset render size; gamescope upscales to `--output` (CS2) |
| `--gamescope-filter fsr\|nis\|linear\|nearest\|…` | Override gamescope `-F` (default from `--upscaler`) |
| `--native` | No upscale — passthrough swapchain |
| `--no-rcas` | Skip sharpening — usually more FPS |
| `--offload` / `--no-offload` | dGPU render, iGPU upscale + present (default: `--offload`) |
| `--one-way` / `--no-one-way` | iGPU present (default with `--offload`) or fused round-trip. One-way skips MangoHud and pins the EdgeScale overlay |
| `--mangohud` | FPS overlay. Not loaded on one-way; use the EdgeScale overlay instead |
| `--controls` / `--no-controls` | In-game hotkeys + status toast (default on) |
| `--overlay` | Pin the status strip at launch (`EDGESCALE_OVERLAY=1`; Proton/Steam often eat F10) |
| `--zink` / `--no-zink` | OpenGL via Zink + viewport/FBO/SDL shim (default: `auto` for OpenGL ELF & `.exe` binaries) |
| `--print` | Show the env edgerun would set, then exit |
| `--no-profile` | Ignore `~/.config/edgescale/profiles.conf` |
| `--caps-shrink` / `--no-caps-shrink` | Allow/forbid smaller `maxImageExtent` (CS2 often needs off) |
| `--adaptive` / `--target-fps N` | Continuous render scale toward a frame-time target |
| `--check` | Smoke-test: confirm proxy vs identity after a short bench |

### Per-game profiles

On first run, `edgerun` copies [share/xfsr/profiles.example.conf](share/xfsr/profiles.example.conf)
to `~/.config/edgescale/profiles.conf` (with a compatibility link under `~/.config/xfsr/`). Matching uses `SteamAppId`, then executable name, then a
command-line substring. CLI flags always override the profile. See [ROADMAP.md](ROADMAP.md) §3.

Auto-pick a preset for the built-in Vulkan load app (or your `SteamAppId`):

```sh
./scripts/xfsr-auto-tune --seconds 5          # recommend
./scripts/xfsr-auto-tune --apply              # write ~/.config/edgescale/profiles.conf
SteamAppId=730 ./scripts/xfsr-auto-tune --apply
```

Adaptive (continuous scale toward a target FPS) and a smoke check:

```sh
edgerun --adaptive --target-fps 60 --upscaler blit --no-rcas --overlay %command%
edgerun --layer-dir ./build --check
```

### In-game controls (default on)

| Key | Action |
|-----|--------|
| **F1** | Cycle preset (**live** via swapchain recreate when the title handles `OUT_OF_DATE`) |
| **F2** | Toggle sharpening (**live**); on `edge`, toggles quality ↔ perf |
| **F3** | Cycle upscaler blit → nearest → NIS → edge (**live** when passes exist) |
| **F4** | Toggle offload same-GPU ↔ round-trip (**live**; brief hitch) |
| **F5** | Toggle one-way preference (**restart** to move WSI) |
| **F10** | Pin / unpin the status overlay (**live**) |

A toast appears for a few seconds after each change (and stays if pinned). Keys are read via X11 and/or `/dev/input` (evdev). Set `EDGESCALE_HUD_MOD=1` if you want **Shift+F*** to avoid clashing with game binds. For Proton titles where F10 does nothing, launch with `--overlay` or:

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
(e.g. 1280×720 on 1080p, or 1920×1080 on 4K). Look for `proxy ready: render=… display=…` or
`mode=proxy-edge` / `hybrid-edge` — not `identity` / “wanted render … identity”.
CS2 profile keeps `caps_shrink=0`; drop `-fullscreen` from launch options.
Opt out of caps clamping with `EDGESCALE_CAPS_SHRINK=0` when needed.

### Environment variables

Prefer **`EDGESCALE_*`** (also accepted: `EDGE_*`). Legacy **`XFSR_*`** names still work.

| Env | Effect |
|-----|--------|
| `EDGESCALE_HUD=0` | Disable in-game controls / overlay |
| `EDGESCALE_OVERLAY=1` | Pin status strip at launch (same as `--overlay`) |
| `EDGESCALE_HUD_MOD=1` | Require Shift+F* for hotkeys (default: bare F-keys) |
| `EDGESCALE_CONTROL=/path` | Override control-file path (default `$XDG_RUNTIME_DIR/xfsr-control`) |
| `EDGESCALE_OUTPUT_W` / `_H` | Present size (same as `--output`) |
| `EDGESCALE_RENDER_W` / `_H` | Internal render size (same as `--render`) |
| `EDGESCALE_IGPU_PRESENT=0` | Force round-trip (dGPU WSI) — same as `--no-one-way` |
| `EDGESCALE_IGPU_PRESENT=1` | One-way iGPU present (any vendor). Skips MangoHud |
| `EDGESCALE_IGPU_PRESENT=force` | One-way even if MangoHud is still loaded (often SIGSEGV) |
| `EDGESCALE_DMABUF=1` | Force dma-buf transfer try (default skips NVIDIA↔Mesa) |
| `EDGESCALE_DMABUF=0` | Never use dma-buf |
| `EDGESCALE_VERBOSE=1` | Log `offload: …`, `transfer backend=…`, `first present ok` |

### OpenGL / Zink

`edgerun` automatically detects OpenGL Linux ELF and Windows PE (`.exe`) binaries and enables Mesa Zink alongside `libxfsr_gl_shim.so` (which remaps `glViewport`, `glScissor`, `glBlitFramebuffer`, and SDL drawable queries). You can also force Zink explicitly with **`--zink`** or disable it with `--no-zink`. Vulkan, DXVK, and Zink share one scale: render = window / preset (or `--render`), present = window / `--output`, windowed or fullscreen. Both native Vulkan and Zink support one-way iGPU presentation (`--offload`, using monotonic opaque swapchain handles) as well as fused asynchronous round-trip offload (`--no-one-way`).

**Wayland:** Vulkan upscale works (shrink-kick). OpenGL/Zink works too — the GL shim shrinks `wl_egl_window` to render size so Zink asks for a shrunk swapchain. `edgerun --zink` still prefers X11 when both are available (often faster on hybrid NVIDIA+AMD); keep pure Wayland with `EDGESCALE_ZINK_WAYLAND=1`. Native VK: `EDGESCALE_VK_WAYLAND=1`.

## Build from source

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DXFSR_BUILD_DEMO=OFF -DXFSR_BUILD_BENCH=OFF
cmake --build build -j --target VkLayer_XFSR
./scripts/install-edge-run.sh          # → ~/.local
./scripts/package-xfsr.sh              # → dist/*.tar.gz
./scripts/package-appimage.sh          # → dist/EdgeScale-*.AppImage
```

Needs CMake ≥ 3.20, C++17, `glslc`, Vulkan loader, and `libx11-dev` (GL shim X11 hooks). AppImage packaging also needs `curl` (to fetch `appimagetool`).

## Versioning

- [`VERSION`](VERSION) — **0.8.3**
- [`CHANGELOG.md`](CHANGELOG.md)
- Tag `vX.Y.Z` → GitHub Actions builds Linux AppImage + tarball and Windows Setup.exe + ZIP

```sh
git tag v0.8.3 && git push origin v0.8.3
```

## Credits

EdgeScale’s primary upscalers (`edge`, `nis`, `blit`, `nearest`) are first-party / openly documented techniques. Optional third-party shader sources may live under `third_party/`.
