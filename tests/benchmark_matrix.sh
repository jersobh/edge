#!/usr/bin/env bash
# Controlled Benchmark Matrix for Cost-Aware Device & Memory Scheduler
# Workloads that EXCEED physical single-GPU VRAM:
#   Test B1: NVIDIA Native, WITHOUT VRAM spoofing (c=32768 -> CRASH / OOM)
#   Test B2: NVIDIA + EdgeScale, WITH VRAM spoofing (c=32768 -> SUCCESS / Host Spill)
#   Test F:  NVIDIA + AMD, layer split + host spill (Multi-GPU oversubscription, c=32768)
set -eo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EDGERUN="${ROOT}/scripts/edgerun"
MODEL_BLOB="/home/jeff/.ollama/models/blobs/sha256-c2db2ec57aab8f97df951f8ccd5dfb6073ce8ab50f8ba8174898ed31d35a8605"
PROMPT="Explain what a GPU is in 20 words."
GEN_TOKENS=40

BOLD='\033[1m'
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BOLD}${BLUE}================================================================================================${NC}"
echo -e "${BOLD}${CYAN}            CONTROLLED BENCHMARK MATRIX: COST-AWARE MULTI-GPU SCHEDULER                         ${NC}"
echo -e "${BOLD}${BLUE}================================================================================================${NC}"
echo -e "Hardware: NVIDIA GeForce GTX 1650 (4GB VRAM) + AMD Radeon Vega 10 (Shared DDR4)"
echo -e "Model:    Qwen 3.5 4.3B (Q4_K - Medium) | Prompt: \"${PROMPT}\""
echo -e "Tokens:   Target = ${GEN_TOKENS} tokens generated\n"

run_and_extract() {
  local logfile
  logfile=$(mktemp)
  setsid "$@" < /dev/null > "$logfile" 2>&1 || true

  # Strip ANSI color codes and carriage returns
  local cleanfile
  cleanfile=$(mktemp)
  sed 's/\x1b\[[0-9;]*[a-zA-Z]//g; s/\r//g' "$logfile" > "$cleanfile"

  local prompt_tps gen_tps spill_res
  if grep -qi "ErrorOutOfDeviceMemory" "$cleanfile"; then
    prompt_tps="CRASH"
    gen_tps="CRASH (OOM)"
    spill_res="OutOfDeviceMemory"
  else
    prompt_tps=$(grep -o "Prompt: *[0-9,.]*" "$cleanfile" | tail -n 1 | awk '{print $2}')
    gen_tps=$(grep -o "Generation: *[0-9,.]*" "$cleanfile" | tail -n 1 | awk '{print $2}')
    local mb
    mb=$(grep -o "spilled *[0-9]* *MB" "$cleanfile" | tail -n 1 | awk '{print $2}')

    [[ -z "$prompt_tps" ]] && prompt_tps="N/A"
    [[ -z "$gen_tps" ]] && gen_tps="N/A"
    if [[ -n "$mb" ]]; then
      spill_res="${mb} MB Spilled"
    else
      spill_res="0 MB (In-VRAM)"
    fi
  fi

  echo "${prompt_tps}|${gen_tps}|${spill_res}"
  rm -f "$logfile" "$cleanfile"
}

# --- TEST A ---
echo -e "${BOLD}${YELLOW}>>> Running Test A: NVIDIA Only (In-VRAM, c=4096)...${NC}"
RES_A=$(run_and_extract llama-cli -m "$MODEL_BLOB" --device Vulkan1 --color off -c 4096 -ngl 99 -p "$PROMPT" -n "$GEN_TOKENS" --single-turn)
P_A=$(echo "$RES_A" | cut -d'|' -f1)
G_A=$(echo "$RES_A" | cut -d'|' -f2)
S_A=$(echo "$RES_A" | cut -d'|' -f3)

# --- TEST B1 ---
echo -e "${BOLD}${YELLOW}>>> Running Test B1: NVIDIA Native WITHOUT Spoofing (c=32768, Expect Crash)...${NC}"
RES_B1=$(run_and_extract llama-cli -m "$MODEL_BLOB" --device Vulkan1 --color off -c 32768 -ngl 99 -p "$PROMPT" -n "$GEN_TOKENS" --single-turn)
P_B1=$(echo "$RES_B1" | cut -d'|' -f1)
G_B1=$(echo "$RES_B1" | cut -d'|' -f2)
S_B1=$(echo "$RES_B1" | cut -d'|' -f3)

# --- TEST B2 ---
echo -e "${BOLD}${YELLOW}>>> Running Test B2: NVIDIA + EdgeScale WITH VRAM Spoofing (c=32768, 8GB Spoof)...${NC}"
RES_B2=$(run_and_extract "$EDGERUN" --spoof-vram 8GB --no-offload -- llama-cli -m "$MODEL_BLOB" --device Vulkan1 --color off -c 32768 -ngl 99 -p "$PROMPT" -n "$GEN_TOKENS" --single-turn)
P_B2=$(echo "$RES_B2" | cut -d'|' -f1)
G_B2=$(echo "$RES_B2" | cut -d'|' -f2)
S_B2=$(echo "$RES_B2" | cut -d'|' -f3)
[[ "$S_B2" == "0 MB (In-VRAM)" ]] && S_B2="Host Spill / OK"

# --- TEST C ---
echo -e "${BOLD}${YELLOW}>>> Running Test C: NVIDIA + AMD Layer Split (No Spill, c=4096)...${NC}"
RES_C=$(run_and_extract llama-cli -m "$MODEL_BLOB" --device Vulkan1,Vulkan0 --color off --split-mode layer --tensor-split 3,1 -c 4096 -ngl 99 -p "$PROMPT" -n "$GEN_TOKENS" --single-turn)
P_C=$(echo "$RES_C" | cut -d'|' -f1)
G_C=$(echo "$RES_C" | cut -d'|' -f2)
S_C=$(echo "$RES_C" | cut -d'|' -f3)

# --- TEST F ---
echo -e "${BOLD}${YELLOW}>>> Running Test F: NVIDIA + AMD Layer Split (Host Spill, c=32768, EdgeScale)...${NC}"
RES_F=$(run_and_extract "$EDGERUN" --spoof-vram 8GB --no-offload -- llama-cli -m "$MODEL_BLOB" --device Vulkan1,Vulkan0 --color off --split-mode layer --tensor-split 3,1 -c 32768 -ngl 99 -p "$PROMPT" -n "$GEN_TOKENS" --single-turn)
P_F=$(echo "$RES_F" | cut -d'|' -f1)
G_F=$(echo "$RES_F" | cut -d'|' -f2)
S_F=$(echo "$RES_F" | cut -d'|' -f3)

format_tps() {
  local val="$1"
  if [[ -z "$val" || "$val" == "N/A" ]]; then
    echo "N/A"
  elif [[ "$val" =~ "CRASH" ]]; then
    echo "$val"
  else
    echo "$val t/s"
  fi
}

echo -e "\n${BOLD}${BLUE}================================================================================================${NC}"
echo -e "${BOLD}${GREEN}                              CONTROLLED BENCHMARK MATRIX RESULTS                               ${NC}"
echo -e "${BOLD}${BLUE}================================================================================================${NC}"
printf "%-8s | %-33s | %-12s | %-14s | %-18s\n" "Test" "Configuration" "Prompt (t/s)" "Decode (t/s)" "Result / Spill"
echo "------------------------------------------------------------------------------------------------"
printf "%-8s | %-33s | %-12s | %-14s | %-18s\n" "Test A"  "NVIDIA In-VRAM (Native 4k)"       "$(format_tps "$P_A")"  "$(format_tps "$G_A")"  "$S_A"
printf "%-8s | %-33s | %-12s | %-14s | %-18s\n" "Test B1" "NVIDIA Native (No Spoof, 32k)"    "$(format_tps "$P_B1")" "$(format_tps "$G_B1")" "$S_B1"
printf "%-8s | %-33s | %-12s | %-14s | %-18s\n" "Test B2" "NVIDIA + EdgeScale (8GB Spoof)"   "$(format_tps "$P_B2")" "$(format_tps "$G_B2")" "$S_B2"
printf "%-8s | %-33s | %-12s | %-14s | %-18s\n" "Test C"  "NVIDIA + AMD (Layer Split, 4k)"   "$(format_tps "$P_C")"  "$(format_tps "$G_C")"  "$S_C"
printf "%-8s | %-33s | %-12s | %-14s | %-18s\n" "Test F"  "Dual GPU + Host Spill (32k)"      "$(format_tps "$P_F")"  "$(format_tps "$G_F")"  "$S_F"
echo -e "${BOLD}${BLUE}================================================================================================${NC}"
