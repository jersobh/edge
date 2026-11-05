#!/usr/bin/env bash
# Runs a local LLM with EdgeScale Hybrid VRAM oversubscription
# Backed by 4 GB GTX 1650 + 32 GB System RAM
set -eo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EDGERUN="${ROOT}/scripts/edgerun"
MODEL_BLOB="/home/jeff/.ollama/models/blobs/sha256-c2db2ec57aab8f97df951f8ccd5dfb6073ce8ab50f8ba8174898ed31d35a8605"
PROMPT="${1:-Explain what Linux is in two sentences.}"
CTX_SIZE="${CTX_SIZE:-32768}"

echo "================================================================================"
echo " Running Local LLM (Qwen 3.5 4.3B GGUF) with EdgeScale Hybrid VRAM Manager"
echo " Context Window : ${CTX_SIZE} tokens (> 4 GB VRAM Footprint)"
echo " Prompt         : \"${PROMPT}\""
echo "================================================================================"

exec "$EDGERUN" --spoof-vram 8GB --no-offload -- \
  llama-cli \
    -m "$MODEL_BLOB" \
    -c "$CTX_SIZE" \
    -ngl 99 \
    -p "$PROMPT" \
    -n 200 \
    --single-turn
