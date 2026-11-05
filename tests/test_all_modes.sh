#!/usr/bin/env bash
# Comprehensive Test Suite for EdgeScale & Hybrid VRAM Manager:
# Validates:
#   1. Memory Observability & Telemetry Daemon (NVML, Sysfs, Residency Scoring)
#   2. AI Memory Virtualization (CUDA HMM / Managed Memory >4GB on GTX 1650)
#   3. Vulkan Memory Virtualization & Budget Spoofing
#   4. All Upscalers (edge [quality, perf, nearest], fsr1, nis, blit, nearest)
#   5. All Execution Modes (same-GPU, offload, hybrid, native, DSR 4K)
set -eo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"
EDGERUN="${ROOT}/scripts/edgerun"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

PASSED_COUNT=0
FAILED_COUNT=0

log_header() {
  echo -e "\n${BOLD}${BLUE}======================================================================${NC}"
  echo -e "${BOLD}${CYAN}  $1${NC}"
  echo -e "${BOLD}${BLUE}======================================================================${NC}"
}

run_test() {
  local name="$1"
  shift
  echo -ne " [TEST] ${BOLD}$name${NC}... "
  local logfile
  logfile=$(mktemp)
  if "$@" > "$logfile" 2>&1; then
    echo -e "${GREEN}${BOLD}PASS${NC}"
    rm -f "$logfile"
    PASSED_COUNT=$((PASSED_COUNT + 1))
  else
    echo -e "${RED}${BOLD}FAIL${NC}"
    echo -e "${RED}--- Error Output ---${NC}"
    tail -n 25 "$logfile"
    echo -e "${RED}--------------------${NC}"
    rm -f "$logfile"
    FAILED_COUNT=$((FAILED_COUNT + 1))
  fi
}

log_header "EdgeScale & Hybrid VRAM Architecture Test Suite"
echo "Host Kernel: $(uname -r)"
echo "Directory:   $ROOT"
echo "Build Dir:   $BUILD"

# Ensure binaries and layer are fresh
log_header "Phase 0: Build Verification & Installation"
run_test "CMake Build All Targets" cmake --build "$BUILD" -j"$(nproc)"
run_test "Install EdgeScale Layer & Shaders" "$ROOT/scripts/install-edge-run.sh"

log_header "Phase 1: Memory Observability & Telemetry Daemon (Pillar 1)"
run_test "VRAM Monitor: Status Snapshot" "$BUILD/vram_monitor" --once
run_test "VRAM Monitor: JSON Telemetry Dump" "$BUILD/vram_monitor" --json
run_test "VRAM Monitor: Residency Scoring Engine" "$BUILD/vram_monitor" --score --freq 10 --reuse 0.95 --cost 5.0 --mb 128 --xfer 0.4

log_header "Phase 2: AI CUDA Virtualization & HMM Oversubscription (Pillar 2)"
run_test "CUDA HMM 5.25 GB Oversubscription on 4GB GTX 1650" "$BUILD/ai_vram_test" --target-gb 5.25 --layers 6

log_header "Phase 3: Vulkan Memory Virtualization & Budget Spoofing (Pillar 3)"
run_test "Vulkan Native Memory Heap Query" "$BUILD/vulkan_vram_test" --target-mb 1024
run_test "Vulkan VRAM Spoofing 8GB via EdgeScale Layer" "$EDGERUN" --spoof-vram 8GB --no-offload -- "$BUILD/vulkan_vram_test" --target-mb 1024

log_header "Phase 4: Upscalers Verification"
# Edge Quality
run_test "Upscaler: Edge (mode=quality)" "$EDGERUN" --upscaler edge --edge-mode quality --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# Edge Perf
run_test "Upscaler: Edge (mode=perf)" "$EDGERUN" --upscaler edge --edge-mode perf --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# Edge Nearest (2D pixel art)
run_test "Upscaler: Edge (mode=nearest)" "$EDGERUN" --upscaler edge --edge-mode nearest --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# FSR 1.0
run_test "Upscaler: FSR 1.0 (EASU + RCAS)" "$EDGERUN" --upscaler fsr1 --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# NIS (NVIDIA Image Scaling)
run_test "Upscaler: NIS (NVIDIA Image Scaling)" "$EDGERUN" --upscaler nis --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# Blit (Linear)
run_test "Upscaler: Blit (Bilinear)" "$EDGERUN" --upscaler blit --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# Nearest
run_test "Upscaler: Nearest (Integer)" "$EDGERUN" --upscaler nearest --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360

log_header "Phase 5: Execution Modes Verification"
# Mode: same-GPU (no offload)
run_test "Mode: Same-GPU (--no-offload)" "$EDGERUN" --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# Mode: offload (dual-GPU: dGPU render -> AMD Vega 10 iGPU upscale & present)
run_test "Mode: Dual-GPU Offload (--offload)" "$EDGERUN" --offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# Mode: hybrid (dGPU render + iGPU EdgePass reconstruct & present)
run_test "Mode: Full Hybrid Offload (--hybrid)" "$EDGERUN" --hybrid -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# Mode: native (passthrough)
run_test "Mode: Native Passthrough (--native)" "$EDGERUN" --native --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360
# Mode: DSR 4K (Supersampling / Downsampling)
run_test "Mode: Supersampling DSR 4K (--dsr 4k)" "$EDGERUN" --dsr 4k --no-offload -- "$BUILD/xfsr_load_vk" --seconds 1 --width 640 --height 360

log_header "Test Summary"
echo -e "Total Passed: ${GREEN}${BOLD}${PASSED_COUNT}${NC}"
echo -e "Total Failed: ${RED}${BOLD}${FAILED_COUNT}${NC}"

if [ "$FAILED_COUNT" -eq 0 ]; then
  echo -e "\n${GREEN}${BOLD}ALL TESTS PASSED SUCCESSFULLY!${NC}\n"
  exit 0
else
  echo -e "\n${RED}${BOLD}SOME TESTS FAILED!${NC}\n"
  exit 1
fi

