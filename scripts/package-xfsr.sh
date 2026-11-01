#!/usr/bin/env bash
# Build a relocatable xfsr-run release tarball under dist/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|amd64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
esac
NAME="edgescale-${VERSION}-linux-${ARCH}"
BUILD="${XFSR_BUILD:-$ROOT/build-release}"
STAGE="${XFSR_STAGE:-$ROOT/dist/.stage/$NAME}"
OUT_DIR="${XFSR_DIST:-$ROOT/dist}"
TAR="$OUT_DIR/${NAME}.tar.gz"

echo "packaging $NAME ..."

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DXFSR_BUILD_DEMO=OFF -DXFSR_BUILD_BENCH=OFF
cmake --build "$BUILD" -j"$(nproc)" --target VkLayer_XFSR xfsr_gl_shim

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share/vulkan/implicit_layer.d" "$STAGE/share/xfsr/shaders"

install -m 755 "$ROOT/scripts/edgerun" "$STAGE/bin/edgerun"
ln -sf edgerun "$STAGE/bin/xfsr-run"
install -m 755 "$ROOT/scripts/xfsr-auto-tune" "$STAGE/bin/xfsr-auto-tune"
ln -sf xfsr-auto-tune "$STAGE/bin/edgescale-auto-tune"
install -m 755 "$ROOT/scripts/xfsr-gamescope-focus" "$STAGE/bin/xfsr-gamescope-focus"
ln -sf xfsr-gamescope-focus "$STAGE/bin/edgescale-gamescope-focus"
install -m 755 "$BUILD/libVkLayer_XFSR.so" "$STAGE/lib/libVkLayer_XFSR.so"
install -m 755 "$BUILD/libxfsr_gl_shim.so" "$STAGE/lib/libxfsr_gl_shim.so"
if [[ -f "$ROOT/share/xfsr/profiles.example.conf" ]]; then
  install -m 644 "$ROOT/share/xfsr/profiles.example.conf" "$STAGE/share/xfsr/profiles.example.conf"
fi

# Layer JSONs with relative library path (resolved from install prefix by install.sh)
python3 - <<PY
import json, pathlib
for name in ["VkLayer_XFSR.json", "VkLayer_EdgeScale.json"]:
    src = pathlib.Path(f"$BUILD/{name}")
    if not src.exists():
        continue
    dst = pathlib.Path(f"$STAGE/share/vulkan/implicit_layer.d/{name}")
    data = json.loads(src.read_text())
    data["layer"]["library_path"] = "../../../lib/libVkLayer_XFSR.so"
    data["layer"]["implementation_version"] = "${VERSION}".split(".")[0]
    dst.write_text(json.dumps(data, indent=2) + "\n")
    print("wrote", dst)
PY

# SPIR-V the layer loads (FSR, NIS, Edge, + blit helpers)
shopt -s nullglob
for f in "$BUILD"/shaders/fsr_*.spv "$BUILD"/shaders/nis_*.spv \
         "$BUILD"/shaders/edge_*.spv "$BUILD"/shaders/blit.*.spv; do
  install -m 644 "$f" "$STAGE/share/xfsr/shaders/"
done
shopt -u nullglob

printf '%s\n' "$VERSION" > "$STAGE/VERSION"
cp -f "$ROOT/CHANGELOG.md" "$STAGE/CHANGELOG.md"
[[ -f "$ROOT/LICENSE" ]] && cp -f "$ROOT/LICENSE" "$STAGE/LICENSE"

# Tiny README inside the tarball
cat > "$STAGE/README.txt" <<EOF
EdgeScale (edgerun) ${VERSION} (linux-${ARCH})

Install:
  ./install-from-tarball.sh
  # or:  PREFIX=\$HOME/.local ./install-from-tarball.sh

Steam launch options:
  edgerun --mangohud --preset performance --no-rcas %command%
  gamemoderun edgerun --offload --mangohud --preset performance --no-rcas %command%

Legacy command alias supported:
  xfsr-run %command%
EOF

# Self-contained installer shipped inside the archive
cat > "$STAGE/install-from-tarball.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
mkdir -p "$PREFIX/bin" "$PREFIX/lib" "$PREFIX/share/vulkan/implicit_layer.d" "$PREFIX/share/xfsr/shaders"
if [[ -f "$HERE/bin/edgerun" ]]; then
  install -m 755 "$HERE/bin/edgerun" "$PREFIX/bin/edgerun"
  ln -sf edgerun "$PREFIX/bin/xfsr-run"
else
  install -m 755 "$HERE/bin/xfsr-run" "$PREFIX/bin/xfsr-run"
  ln -sf xfsr-run "$PREFIX/bin/edgerun"
fi
[[ -f "$HERE/bin/xfsr-auto-tune" ]] && install -m 755 "$HERE/bin/xfsr-auto-tune" "$PREFIX/bin/xfsr-auto-tune"
[[ -f "$PREFIX/bin/xfsr-auto-tune" ]] && ln -sf xfsr-auto-tune "$PREFIX/bin/edgescale-auto-tune"
[[ -f "$HERE/bin/xfsr-gamescope-focus" ]] && install -m 755 "$HERE/bin/xfsr-gamescope-focus" "$PREFIX/bin/xfsr-gamescope-focus"
[[ -f "$PREFIX/bin/xfsr-gamescope-focus" ]] && ln -sf xfsr-gamescope-focus "$PREFIX/bin/edgescale-gamescope-focus"
install -m 755 "$HERE/lib/libVkLayer_XFSR.so" "$PREFIX/lib/libVkLayer_XFSR.so"
install -m 755 "$HERE/lib/libxfsr_gl_shim.so" "$PREFIX/lib/libxfsr_gl_shim.so"
cp -a "$HERE/share/xfsr/shaders/." "$PREFIX/share/xfsr/shaders/"
if [[ -f "$HERE/share/xfsr/profiles.example.conf" ]]; then
  install -m 644 "$HERE/share/xfsr/profiles.example.conf" "$PREFIX/share/xfsr/profiles.example.conf"
  mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/edgescale" "${XDG_CONFIG_HOME:-$HOME/.config}/xfsr"
  if [[ ! -f "${XDG_CONFIG_HOME:-$HOME/.config}/edgescale/profiles.conf" ]]; then
    install -m 644 "$HERE/share/xfsr/profiles.example.conf" "${XDG_CONFIG_HOME:-$HOME/.config}/edgescale/profiles.conf"
  fi
  if [[ ! -f "${XDG_CONFIG_HOME:-$HOME/.config}/xfsr/profiles.conf" ]]; then
    ln -sf "${XDG_CONFIG_HOME:-$HOME/.config}/edgescale/profiles.conf" "${XDG_CONFIG_HOME:-$HOME/.config}/xfsr/profiles.conf" 2>/dev/null || cp -f "$HERE/share/xfsr/profiles.example.conf" "${XDG_CONFIG_HOME:-$HOME/.config}/xfsr/profiles.conf"
  fi
fi
python3 - <<PY
import json, os, pathlib
lib = pathlib.Path("$PREFIX/lib/libVkLayer_XFSR.so").resolve()
for name in ["VkLayer_XFSR.json", "VkLayer_EdgeScale.json"]:
    src = pathlib.Path(f"$HERE/share/vulkan/implicit_layer.d/{name}")
    if not src.exists():
        continue
    dst = pathlib.Path(f"$PREFIX/share/vulkan/implicit_layer.d/{name}")
    data = json.loads(src.read_text())
    data["layer"]["library_path"] = os.path.relpath(lib, dst.parent)
    dst.write_text(json.dumps(data, indent=2) + "\n")
    print("wrote", dst)
PY
echo "Installed EdgeScale (edgerun / xfsr-run) to $PREFIX"
echo "Ensure PATH contains $PREFIX/bin"
echo "Steam:  edgerun --mangohud --preset performance --no-rcas %command%"
EOF
chmod +x "$STAGE/install-from-tarball.sh"

mkdir -p "$OUT_DIR"
tar -C "$(dirname "$STAGE")" -czf "$TAR" "$(basename "$STAGE")"
rm -rf "${ROOT}/dist/.stage"

# Convenience copy without version (local dist/)
cp -f "$TAR" "$OUT_DIR/edgescale-linux-${ARCH}.tar.gz"

ls -lh "$TAR" "$OUT_DIR/edgescale-linux-${ARCH}.tar.gz"
echo "OK $TAR"
