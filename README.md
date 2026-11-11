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

**Current version:** [0.13.1](https://github.com/jersobh/edge/releases/tag/v0.13.1)  
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
edgerun --dsr 4k --spoof-vram 8GB %command%
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

### Supersampling / DSR (4K on a 1080p Display)

EdgeScale also supports supersampling / downsampling (Virtual Super Resolution / DSR / SSAA):
```text
edgerun --dsr 4k %command%
edgerun --dsr 1440p %command%
```
The game renders internally at full 4K (3840×2160) or 1440p, and EdgeScale downsamples to your 1080p display using a multi-tap box filter + Contrast-Adaptive Sharpening, delivering pristine anti-aliased geometry and micro-detail with zero shimmering.

### VRAM Capacity Spoofing & Oversubscription

For GPUs with 4GB or 6GB of VRAM where games artificially lock out High / Ultra textures or fail budget queries:
```text
edgerun --spoof-vram 8GB %command%
edgerun --spoof-vram 12GB %command%
```
- **Heap & Budget Spoofing:** Intercepts `vkGetPhysicalDeviceMemoryProperties`, `vkGetPhysicalDeviceMemoryProperties2`, and `VK_EXT_memory_budget` to present a larger local VRAM capacity to the game.
- **Graceful Host Spilling:** Intercepts `vkAllocateMemory` — if physical local VRAM is exhausted (`VK_ERROR_OUT_OF_DEVICE_MEMORY`), it automatically falls back to allocating over PCIe in system RAM (`HOST_VISIBLE`) instead of crashing the game.
*(Note: Pushing textures far beyond physical VRAM capacity can cause PCIe bus stutter).*

`--no-rcas` disables the optional sharpening pass on upscalers that support it — often a bit more FPS, slightly softer image.

## AI & Distributed GPU Memory Management

Beyond graphics upscaling, EdgeScale provides a tiered framework for **distributed GPU memory management and workload orchestration across heterogeneous devices** (e.g. discrete NVIDIA dGPU + integrated AMD Radeon iGPU + host system RAM). Rather than attempting to merge separate physical devices into an unsupported unified hardware device (which driver isolation and NUMA memory domains prevent), EdgeScale coordinates memory placement, residency tracking, and execution across independent devices.

### Architectural Hierarchy

1. **Tier 1 — Dedicated VRAM (Local dGPU):** Highest priority. Latency-sensitive compute, active working buffers, and primary model weights.
2. **Tier 2 — Secondary GPU Memory (iGPU / Secondary dGPU):** Distributed compute tier. Offloads model layers or tensor partitions to secondary devices directly accessing shared DDR4 or secondary VRAM.
3. **Tier 3 — Host System RAM:** Fallback tier for cold weights, extended KV caches, and oversubscription staging across PCIe.
4. **Tier 4 — Storage (NVMe SSD):** Cold assets, checkpointing, and prefetch streaming.

### Memory Manager vs. Execution Orchestrator

EdgeScale strictly separates two responsibilities:
- **Memory Manager:** Intercepts Vulkan and CUDA allocations, manages heap fallbacks (`DEVICE_LOCAL` -> `HOST_VISIBLE`), and tracks cross-device residency.
- **Execution Orchestrator & Cost Scheduler:** Evaluates device topology, memory bandwidth, compute throughput, transfer latency, and access frequency to dynamically schedule tensor/layer partitions.

### Controlled Benchmark Matrix (GTX 1650 4GB + AMD Vega 10 iGPU + 32GB RAM)

EdgeScale is designed specifically for workloads that **exceed physical VRAM**. Benchmarking small models that already fit 100% into native VRAM is trivial; the value of EdgeScale is overcoming the VRAM wall when native execution either crashes with Out-of-Memory or degrades to CPU compute.

#### 1. Beyond-VRAM Workload: TwIL-8B (Q4_K_M — 4.9 GB Model on 4GB dGPU)

When a model exceeds physical VRAM, native execution either hard crashes (`-ngl 33`) or forces you to drop layers onto the slow CPU (`-ngl 15`). EdgeScale provides two solutions: transparent memory virtualization via host spill, or automatic dual-GPU orchestration offloading 100% of layers into GPU memory:

| Scenario | Mode | Layer Distribution | Prompt (t/s) | Generation (t/s) | Result / Memory Status |
|:---|:---|:---:|:---:|:---:|:---|
| **1. Native Single GPU** | `llama-cli (-ngl 33)` | Attempted 33/33 on dGPU | **CRASH** | **CRASH** | `ErrorOutOfDeviceMemory` (Hard OOM) |
| **2. llama.cpp CPU/RAM Offload** | `llama-cli (-ngl 15)` | 15 on dGPU, 18 on CPU | 5.6 t/s | 4.3 t/s | CPU bottleneck (DDR4 host compute) |
| **3. EdgeScale Virtualized** | `edgerun --spoof-vram 8GB` | 33 on dGPU (PCIe Spill) | 6.6 t/s | 1.4 t/s | **Succeeds without crash** (1,013 MB spilled over PCIe) |
| **4. EdgeScale Dual-GPU** | `edgerun` (Auto Orchestrated) | **22 on dGPU, 11 on iGPU** | **42.7 t/s** | **7.4 t/s** | **In-VRAM across both GPUs** (+662% prompt, +72% decode vs CPU) |

#### 2. Context Stress & Memory Virtualization Matrix ($c=32,768$)

When extended context windows blow past physical VRAM limits:

| Test | Configuration | Prompt (t/s) | Decode (t/s) | Result / Spill | Key Takeaway |
|:---|:---|:---:|:---:|:---:|:---|
| **Native Single GPU (All GPU)** | NVIDIA Native ($c=32768$, No Spoof) | **CRASH** | **CRASH** | `OutOfDeviceMemory` | Native driver hard OOM crash |
| **llama.cpp CPU/RAM Offload** | NVIDIA Native + Host RAM ($c=32768$, Partial) | 12.1 t/s | 2.8 t/s | 0 MB (CPU Fallback) | Massive context processing slowdown |
| **EdgeScale Virtualized** | NVIDIA + EdgeScale ($c=32768$, 8GB Spoof) | 40.4 t/s | 9.6 t/s | Host Spill / OK | **Virtualization succeeds** without OOM |
| **EdgeScale Dual-GPU** | NVIDIA + AMD Layer Split + Spill ($c=32768$) | 23.3 t/s | 3.6 t/s | 901 MB Spilled | Multi-tier orchestration under extreme context |

### Tools & Telemetry

- **VRAM Monitor (`vram_monitor`)**: Cross-vendor CLI and background daemon monitoring NVIDIA (NVML), AMD (sysfs VRAM/GTT), and Linux host memory (`/proc/meminfo`) with automated residency scoring:
  ```sh
  ./build/vram_monitor --once
  ./build/vram_monitor --score
  ./build/vram_monitor --json
  ```
- **Local LLM Execution**:
  ```sh
  # Run full dual-GPU layer offload automatically:
  edgerun llama-cli -hf webAI-Official/TwIL-8B --hf-file TwIL-8B-Q4_K_M-balanced.gguf -c 4096 -ngl 40
  # Or run any Vulkan/CUDA AI workload with virtualized memory:
  edgerun --spoof-vram 8GB --no-offload -- llama-cli -m model.gguf -c 32768 -ngl 99
  ```
- **Controlled Matrix Benchmark**:
  ```sh
  ./tests/benchmark_matrix.sh
  ```

## Install

### Linux (AppImage — recommended)

1. Download `EdgeScale-x86_64.AppImage` from [Releases](https://github.com/jersobh/edge/releases/latest).
2. Make it executable:

```sh
chmod +x EdgeScale-x86_64.AppImage
```

3. **Install to system (`~/.local`)**:
```sh
./EdgeScale-x86_64.AppImage --install
```
This registers the Vulkan implicit layer (`VkLayer_EdgeScale.json`), installs `edgerun` into `~/.local/bin`, extracts shaders, sets up desktop integration, and configures default profiles.

To uninstall:
```sh
./EdgeScale-x86_64.AppImage --uninstall
```

**Or run portably without installing**:
```sh
./EdgeScale-x86_64.AppImage --preset performance %command%
```
You can also symlink the AppImage directly to your `PATH`:
```sh
mkdir -p ~/.local/bin
ln -sf "$(pwd)/EdgeScale-x86_64.AppImage" ~/.local/bin/edgerun
```

A versioned `.tar.gz` is also provided under Releases for custom packaging.

### Windows (Setup.exe — recommended)

1. Download **`EdgeScale-*-Setup.exe`** from [Releases](https://github.com/jersobh/edge/releases/latest).
2. Run the installer:
   - Installs per-user to `%LOCALAPPDATA%\EdgeScale` and registers the Vulkan implicit layer in the registry.
   - **Recommended:** Check the box **"Set user environment variable ENABLE_EDGESCALE=1"** during setup to enable EdgeScale globally for all Vulkan games.
3. Uninstall anytime via **Windows Settings → Apps** (or Start Menu → Uninstall EdgeScale).

*Fallback:* Extract `edgescale-windows-x86_64.zip` and run `install-windows.bat` / `uninstall-windows.bat`.

#### Enabling EdgeScale on Windows

Because EdgeScale is an implicit Vulkan layer, it activates when the environment variable `ENABLE_EDGESCALE=1` is set:

- **System-Wide / All Games (Recommended):**  
  If you didn't check the installer box, open Command Prompt or PowerShell once:
  ```cmd
  setx ENABLE_EDGESCALE 1
  setx EDGESCALE_OVERLAY 1
  ```
  *(Restart Steam or your game after running this so the new environment variables take effect).*

- **Per-Game in Steam (Windows):**  
  Right-click the game in Steam → **Properties...** → **Launch Options**:
  ```cmd
  cmd /c "set ENABLE_EDGESCALE=1&& set EDGESCALE_OVERLAY=1&& %command%"
  ```

- **Per-Session in Terminal:**
  ```cmd
  set ENABLE_EDGESCALE=1
  set EDGESCALE_OVERLAY=1
  game.exe
  ```

#### Important: Vulkan vs. DirectX on Windows

> [!IMPORTANT]
> EdgeScale is an in-process **Vulkan layer**. On Windows, games run on **DirectX (Direct3D 9/11/12)** by default, not Vulkan!
> 
> - **Native Vulkan Games (Works out of the box):**  
>   Games that natively support Vulkan run with EdgeScale directly. Examples:  
>   *Counter-Strike 2*, *DOOM (2016)*, *DOOM Eternal*, *Baldur's Gate 3* (select Vulkan in the launcher), *No Man's Sky*, *Red Dead Redemption 2* (switch API to Vulkan in Graphics settings), *Rainbow Six Siege* (launch `RainbowSix_Vulkan.exe`), *The Talos Principle*, *Dota 2*.
> - **DirectX 9 / 10 / 11 Games (Use DXVK):**  
>   To use EdgeScale with DirectX games on Windows, download the latest 64-bit [DXVK release](https://github.com/doitsujin/dxvk/releases) and place `dxgi.dll` and `d3d11.dll` (or `d3d9.dll`) directly in the same folder as the game's executable. DXVK routes rendering through Vulkan, where EdgeScale intercepts and upscales frames.

#### In-Game Hotkeys & HUD Overlay

- **F10**: **Pin / Unpin HUD Overlay**. *(The overlay is unpinned by default unless `EDGESCALE_OVERLAY=1` is set; press `F10` to keep the stats strip on screen).*
- **F1**: Cycle scaling preset (`quality` 1.5× → `balanced` 1.7× → `performance` 2.0× → `ultra` 2.5×)
- **F2**: Cycle EdgePass mode (`quality` → `perf` → `nearest` pixel art) or toggle RCAS sharpening
- **F3**: Cycle spatial upscaler (`edge` → `fsr1` → `blit` → `nearest` → `nis`)

#### Quick Smoke Test
Verify your Windows installation in seconds:
```cmd
set ENABLE_EDGESCALE=1
set EDGESCALE_OVERLAY=1
vkcube
```
*(Requires the Vulkan SDK or runtime installed; `vkcube` will spin with the EdgeScale HUD strip overlaid across the top).*

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
| `--dsr 4k\|1440p\|<val>` | Supersampling / Virtual Super Resolution downsampling (e.g. 4K on 1080p display) |
| `--spoof-vram [SIZE]` | Spoof reported VRAM capacity (e.g. `8GB`, `12GB`, `auto`) and enable host RAM spilling |
| `--blit` | Same scale, linear blit (no spatial upscaler) |
| `--upscaler blit\|nearest\|nis\|edge\|fsr1` | Spatial upscaler (`nearest` = pixel art; `edge` = recommended) |
| `--edge-mode quality\|perf\|nearest` | EdgePass mode: quality (sub-pixel edge tangent + CAS), perf/lite, or nearest (2D pixel art) |
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
| `--fan auto\|opt\|balanced\|max\|quiet\|<pct>` | Hardware fan control via `nbfc` EC (auto, dynamic temp-reactive, 75%, 100%, 50%) |
| `--hotkeys shift\|alt\|ctrl\|ctrl_alt\|nav\|bare\|none` | In-game hotkey modifier mode (default: `shift` to prevent in-game vote clashing) |
| `--no-hotkeys` | Disable keyboard hotkey listeners completely (safest for competitive esports) |
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

To prevent games from misinterpreting in-game hotkeys (e.g. voting `F1`/`F2` or autobuy `F3` in *Counter-Strike 2*), EdgeScale defaults to **`Shift+F*`** hotkeys. You can also select the navigation cluster (`--hotkeys nav`) or disable hotkeys (`--no-hotkeys`).

| Shift Combo | Nav Cluster | Action |
|-------------|-------------|--------|
| **Shift+F1** | **Insert** | Cycle preset (**live** via swapchain recreate when the title handles `OUT_OF_DATE`) |
| **Shift+F2** | **Home** | Toggle mode (**live**); on `edge`, cycles `quality` ↔ `perf` ↔ `nearest` ↔ `competitive` |
| **Shift+F3** | **PageUp** | Cycle upscaler `edge` → `fsr1` → `blit` → `nearest` → `nis` (**live**) |
| **Shift+F4** | **Delete** | Toggle offload same-GPU ↔ round-trip (**live**; brief hitch) |
| **Shift+F5** | **End** | Toggle one-way preference (**restart** to move WSI) |
| **Shift+F6** | **PageDown** | Cycle fan control (**live**): `AUTO` ↔ `AUTO-OPT` ↔ `BALANCED (75%)` ↔ `MAX (100%)` ↔ `QUIET (50%)` |
| **Shift+F10** | **Pause** | Pin / unpin the status overlay (**live**) |

A toast appears for a few seconds after each change (and stays if pinned). Keys are read via X11, Win32 `GetAsyncKeyState`, and/or `/dev/input` (evdev). Configure with `--hotkeys <shift|alt|ctrl|ctrl_alt|nav|bare|none>` or `--no-hotkeys`. For Proton titles where F10 does nothing, launch with `--overlay` or use the control file:

```sh
echo overlay 1 > "$XDG_RUNTIME_DIR/edgescale-control"
echo preset ultra > "$XDG_RUNTIME_DIR/edgescale-control"
echo edge_mode nearest > "$XDG_RUNTIME_DIR/edgescale-control"
echo hotkeys nav > "$XDG_RUNTIME_DIR/edgescale-control"
```
*(Legacy `$XDG_RUNTIME_DIR/xfsr-control` remains symlinked and fully functional).*

Commands: `preset …`, `cycle_preset`, `rcas 0|1`, `edge_sharp 0..1`, `edge_mode quality|perf|nearest|competitive`,
`fsr 0|1`, `upscaler …`, `offload 0|1`, `one-way 0|1`, `fan auto|opt|balanced|max|quiet|<pct>`, `hotkeys shift|alt|ctrl|nav|bare|none`, `overlay 0|1`, `status`.
`offload` applies live (round-trip); `one-way` needs a restart.

**Exclusive fullscreen / D3D9 / CS2:** Many titles create a full-desktop swapchain and
ignore caps shrink → **identity** (looks unchanged; no upscale). For a real proxy:
use **borderless windowed** and set an **in-game resolution below the desktop**
(e.g. 1280×720 on 1080p, or 1920×1080 on 4K). Look for `proxy ready: render=… display=…` or
`mode=proxy-edge` / `hybrid-edge` — not `identity` / “wanted render … identity”.
CS2 profile keeps `caps_shrink=0`; drop `-fullscreen` from launch options.
Opt out of caps clamping with `EDGESCALE_CAPS_SHRINK=0` when needed.

### Environment variables

EdgeScale uses **`EDGESCALE_*`** (or `EDGE_*`) environment variables. Legacy **`XFSR_*`** names remain fully supported aliases for backwards compatibility.

| Variable | Description |
|----------|-------------|
| `ENABLE_EDGESCALE=1` | Enable EdgeScale Vulkan layer (Windows & Linux; alias `ENABLE_XFSR=1`) |
| `EDGESCALE_PRESET` | Preset mode: `quality` (1.5×), `balanced` (1.7×), `performance` (2.0×), `ultra` (2.5×) |
| `EDGESCALE_UPSCALER` | Active upscaler: `edge`, `fsr1`, `nis`, `blit`, or `nearest` |
| `EDGESCALE_EDGE_MODE` | EdgePass mode: `quality` (sub-pixel tangent + CAS), `perf` (lite), or `nearest` (2D pixel art) |
| `EDGESCALE_EDGE_SHARPNESS` | EdgePass post-scale sharpness (0.0 to 1.0, default 0.60) |
| `EDGESCALE_RCAS` | Contrast-Adaptive Sharpening pass toggle (`1` = on, `0` = off) |
| `EDGESCALE_OFFLOAD` | Dual-GPU companion offload (`1` = on by default, `0` = same-GPU) |
| `EDGESCALE_IGPU_PRESENT=1` | One-way direct iGPU presentation (default with offload; zero back-haul transfer) |
| `EDGESCALE_IGPU_PRESENT=0` | Force fused asynchronous round-trip offload to dGPU WSI |
| `EDGESCALE_IGPU_PRESENT=force` | One-way even if MangoHud is still loaded (warning: may SIGSEGV with MangoHud) |
| `EDGESCALE_OUTPUT_W` / `_H` | Target display / presentation resolution (overrides auto-detection) |
| `EDGESCALE_RENDER_W` / `_H` | Internal rendering resolution (overrides preset calculation) |
| `EDGESCALE_DSR` | Downsampling / Virtual Super Resolution target preset (`4k`, `1440p`, or scale factor) |
| `EDGESCALE_SPOOF_VRAM` | Spoofed VRAM capacity reported to Vulkan apps (`8GB`, `12GB`, `16GB`, `auto`) |
| `EDGESCALE_HUD=0` | Disable in-game hotkeys and toast messages |
| `EDGESCALE_OVERLAY=1` | Pin HUD status strip at launch (same as `--overlay`) |
| `EDGESCALE_HUD_MOD=1` | Require Shift+F* for hotkeys (default: bare F-keys) |
| `EDGESCALE_CONTROL=/path` | Override control-file path (default `$XDG_RUNTIME_DIR/edgescale-control`) |
| `EDGESCALE_CAPS_SHRINK=0` | Disable surface extent clamping for stubborn engines |
| `EDGESCALE_ADAPTIVE=1` | Enable dynamic frame-time scaling toward target framerate |
| `EDGESCALE_TARGET_FPS=60` | Target framerate for adaptive resolution scaling |
| `EDGESCALE_DMABUF=1` | Force dma-buf zero-copy transfer try |
| `EDGESCALE_DMABUF=0` | Disable dma-buf buffer imports |
| `EDGESCALE_VERBOSE=1` | Verbose log output (`offload: …`, `first present ok`, proxy extents) |
| `EDGESCALE_ZINK=1` | Force Mesa Zink OpenGL-to-Vulkan translation with GL shim |

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

- [`VERSION`](VERSION) — **0.13.1**
- [`CHANGELOG.md`](CHANGELOG.md)
- Tag `vX.Y.Z` → GitHub Actions builds Linux AppImage + tarball and Windows Setup.exe + ZIP

```sh
git tag v0.13.1 && git push origin v0.13.1
```

## Credits

EdgeScale’s primary upscalers (`edge`, `nis`, `blit`, `nearest`) are first-party / openly documented techniques. Optional third-party shader sources may live under `third_party/`.
