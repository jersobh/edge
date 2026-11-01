#!/usr/bin/env bash
# Cross-compile EdgeScale Windows DLL on Linux using portable Zig or MinGW.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ZIG="${ZIG:-/tmp/zig-linux-x86_64-0.13.0/zig}"
if [[ ! -x "$ZIG" ]]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
  else
    echo "Downloading portable Zig compiler..."
    curl -sL https://ziglang.org/download/0.13.0/zig-linux-x86_64-0.13.0.tar.xz | tar -xJ -C /tmp/
    ZIG="/tmp/zig-linux-x86_64-0.13.0/zig"
  fi
fi

TARGET="x86_64-windows-gnu"
DIST="$ROOT/dist/edgescale-windows-x86_64"
OBJ="$DIST/obj"
mkdir -p "$OBJ" "$DIST/shaders"

echo "[1/4] Compiling shaders..."
cmake --build "$ROOT/build" --target shaders

echo "[2/4] Compiling C source files..."
$ZIG cc -target "$TARGET" -O2 -c \
  -DVK_NO_PROTOTYPES \
  -Isrc -Ilayer -Ibuild/_deps/vulkan_headers-src/include -Ibuild/_deps/volk-src \
  layer/xfsr_layer_entry.c -o "$OBJ/xfsr_layer_entry.o"

$ZIG cc -target "$TARGET" -O2 -c \
  -DVK_NO_PROTOTYPES \
  -DvkGetInstanceProcAddr=volk_vkGetInstanceProcAddr \
  -DvkGetDeviceProcAddr=volk_vkGetDeviceProcAddr \
  -Isrc -Ilayer -Ibuild/_deps/vulkan_headers-src/include -Ibuild/_deps/volk-src \
  build/_deps/volk-src/volk.c -o "$OBJ/volk.o"

echo "[3/4] Compiling C++ source files..."
for src in src/dual_devices.cpp src/frame_transfer.cpp src/fsr_pass.cpp src/nis_pass.cpp \
           src/edge_pass.cpp src/gpu_image.cpp src/offload_roundtrip.cpp src/present.cpp \
           layer/xfsr_layer.cpp layer/xfsr_controls.cpp; do
  base="$(basename "$src" .cpp)"
  echo "  -> $src"
  $ZIG c++ -target "$TARGET" -O2 -std=c++17 -c \
    -DVK_NO_PROTOTYPES \
    -DvkGetInstanceProcAddr=volk_vkGetInstanceProcAddr \
    -DvkGetDeviceProcAddr=volk_vkGetDeviceProcAddr \
    -Isrc -Ilayer -Ibuild/_deps/vulkan_headers-src/include -Ibuild/_deps/volk-src -Ithird_party/ffx-fsr \
    "$src" -o "$OBJ/${base}.o"
done

echo "[4/4] Linking VkLayer_XFSR.dll..."
$ZIG c++ -target "$TARGET" -shared -O2 \
  "$OBJ"/*.o \
  -luser32 \
  -o "$DIST/VkLayer_XFSR.dll"

# Staging Windows package
cp -f "$ROOT/build/shaders"/*.spv "$DIST/shaders/"
cp -f "$ROOT/scripts/install-windows.bat" "$DIST/"
cp -f "$ROOT/scripts/uninstall-windows.bat" "$DIST/"
cp -f "$ROOT/README.md" "$DIST/"

# Write JSON manifests with relative Windows paths
python3 - <<PY
import json, pathlib
for name, env_en in [("VkLayer_XFSR.json", "ENABLE_XFSR"), ("VkLayer_EdgeScale.json", "ENABLE_EDGESCALE")]:
    dst = pathlib.Path("$DIST") / name
    data = {
        "file_format_version": "1.2.0",
        "layer": {
            "name": name.split(".")[0].upper(),
            "type": "GLOBAL",
            "library_path": ".\\\\VkLayer_XFSR.dll",
            "api_version": "1.3.0",
            "implementation_version": "8",
            "description": "EdgeScale in-process Vulkan upscaler & companion offload",
            "functions": {
                "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion"
            },
            "enable_environment": { env_en: "1" },
            "disable_environment": { f"DIS{env_en[2:]}": "1" }
        }
    }
    dst.write_text(json.dumps(data, indent=2) + "\n")
    print(f"wrote {dst}")
PY

# Package ZIP
VER="$(tr -d '[:space:]' < "$ROOT/VERSION")"
ZIP="$ROOT/dist/edgescale-${VER}-windows-x86_64.zip"
python3 - <<PY
import zipfile, pathlib
dist = pathlib.Path("$DIST")
zip_path = pathlib.Path("$ZIP")
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
    for path in dist.rglob("*"):
        if "obj" in path.parts:
            continue
        if path.is_file():
            zf.write(path, path.relative_to(dist))
print("Wrote ZIP:", zip_path)
PY
cp -f "$ZIP" "$ROOT/dist/edgescale-windows-x86_64.zip"

echo "Windows build complete:"
echo "  DLL:     $DIST/VkLayer_XFSR.dll"
echo "  Archive: $ZIP"
