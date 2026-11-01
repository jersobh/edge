#!/usr/bin/env bash
# Install xfsr-run + layer into the user profile from a local build (no root).
#   ~/.local/bin/xfsr-run
#   ~/.local/lib/libVkLayer_XFSR.so
#   ~/.local/share/vulkan/implicit_layer.d/VkLayer_XFSR.json
#   ~/.local/share/xfsr/shaders/*.spv
#
# Prefer the one-liner for binary installs:
#   curl -fsSL https://raw.githubusercontent.com/jersobh/edge/main/install.sh | bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${XFSR_BUILD:-$ROOT/build}"
PREFIX="${PREFIX:-$HOME/.local}"

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DXFSR_BUILD_DEMO=OFF -DXFSR_BUILD_BENCH=OFF
cmake --build "$BUILD" -j"$(nproc)" --target VkLayer_XFSR xfsr_gl_shim

mkdir -p "$PREFIX/bin" "$PREFIX/lib" \
  "$PREFIX/share/vulkan/implicit_layer.d" \
  "$PREFIX/share/xfsr/shaders"

install -m 755 "$ROOT/scripts/edgerun" "$PREFIX/bin/edgerun"
ln -sf edgerun "$PREFIX/bin/xfsr-run"
install -m 755 "$ROOT/scripts/xfsr-auto-tune" "$PREFIX/bin/xfsr-auto-tune"
ln -sf xfsr-auto-tune "$PREFIX/bin/edgescale-auto-tune"
install -m 755 "$ROOT/scripts/xfsr-gamescope-focus" "$PREFIX/bin/xfsr-gamescope-focus"
ln -sf xfsr-gamescope-focus "$PREFIX/bin/edgescale-gamescope-focus"
install -m 755 "$BUILD/libVkLayer_XFSR.so" "$PREFIX/lib/libVkLayer_XFSR.so"
install -m 755 "$BUILD/libxfsr_gl_shim.so" "$PREFIX/lib/libxfsr_gl_shim.so"
if [[ -f "$ROOT/share/xfsr/profiles.example.conf" ]]; then
  install -m 644 "$ROOT/share/xfsr/profiles.example.conf" \
    "$PREFIX/share/xfsr/profiles.example.conf"
fi
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/edgescale" "${XDG_CONFIG_HOME:-$HOME/.config}/xfsr"
if [[ ! -f "${XDG_CONFIG_HOME:-$HOME/.config}/edgescale/profiles.conf" \
      && -f "$ROOT/share/xfsr/profiles.example.conf" ]]; then
  install -m 644 "$ROOT/share/xfsr/profiles.example.conf" \
    "${XDG_CONFIG_HOME:-$HOME/.config}/edgescale/profiles.conf"
fi

# Drop pre-rename layer artifacts so the loader does not load two XFSR layers.
rm -f "$PREFIX/lib/libVkLayer_CROSSVULKAN_xfsr.so" \
  "$PREFIX/share/vulkan/implicit_layer.d/VkLayer_CROSSVULKAN_xfsr.json"

# FSR (+ blit) SPIR-V the layer loads via XFSR_SHADER_DIR
shopt -s nullglob
for f in "$BUILD"/shaders/fsr_*.spv "$BUILD"/shaders/nis_*.spv \
         "$BUILD"/shaders/edge_*.spv "$BUILD"/shaders/blit.*.spv; do
  install -m 644 "$f" "$PREFIX/share/xfsr/shaders/"
done
shopt -u nullglob

python3 - <<PY
import json, pathlib, os
lib = pathlib.Path("$PREFIX/lib/libVkLayer_XFSR.so").resolve()
for name in ["VkLayer_XFSR.json", "VkLayer_EdgeScale.json"]:
    src = pathlib.Path(f"$BUILD/{name}")
    if not src.exists():
        continue
    dst = pathlib.Path(f"$PREFIX/share/vulkan/implicit_layer.d/{name}")
    data = json.loads(src.read_text())
    # Relative to the JSON dir so pressure-vessel path rewrites stay valid.
    data["layer"]["library_path"] = os.path.relpath(lib, dst.parent)
    # Keep api_version conservative for older Steam loaders.
    data["layer"]["api_version"] = data["layer"].get("api_version", "1.3.0")
    dst.write_text(json.dumps(data, indent=2) + "\n")
    print("wrote", dst)
print("library_path =", os.path.relpath(lib, dst.parent))
# Show glibc deps — anything > 2.31 needs PV host-lib RPATH to load in Proton.
import subprocess
out = subprocess.check_output(["objdump", "-T", str(lib)], text=True, stderr=subprocess.DEVNULL)
vers = sorted({line.split()[-2] for line in out.splitlines() if "GLIBC_" in line and "*UND*" in line})
print("glibc deps:", ", ".join(vers) if vers else "(none)")
PY

echo
echo "Installed EdgeScale:"
echo "  $PREFIX/bin/edgerun  (alias: xfsr-run)"
echo "  $PREFIX/lib/libVkLayer_XFSR.so"
echo "  $PREFIX/lib/libxfsr_gl_shim.so"
echo "  $PREFIX/share/vulkan/implicit_layer.d/VkLayer_XFSR.json"
echo "  $PREFIX/share/xfsr/shaders/"
echo
if ! command -v edgerun >/dev/null 2>&1; then
  echo "Add to PATH if needed:  export PATH=\"$PREFIX/bin:\$PATH\""
fi
echo "Profiles:             ~/.config/edgescale/profiles.conf"
echo "Auto-tune:            edgescale-auto-tune --apply"
echo "Roadmap:              $ROOT/ROADMAP.md"
echo "Steam launch option:  edgerun --preset performance %command%"
echo "                      (or: xfsr-run --preset performance %command%)"
echo "Smoke test:           edgerun --print true"
