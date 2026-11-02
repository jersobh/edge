#!/usr/bin/env bash
# Build a portable EdgeScale AppImage under dist/
# Depends on: scripts/package-xfsr.sh stage layout (or builds via that script).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|amd64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
  *)
    echo "package-appimage: unsupported arch $ARCH" >&2
    exit 1
    ;;
esac

OUT_DIR="${XFSR_DIST:-$ROOT/dist}"
TAR="$OUT_DIR/edgescale-${VERSION}-linux-${ARCH}.tar.gz"
APPDIR="$OUT_DIR/.appdir/EdgeScale.AppDir"
APPIMAGE_NAME="EdgeScale-${VERSION}-${ARCH}.AppImage"
APPIMAGE_OUT="$OUT_DIR/$APPIMAGE_NAME"

if [[ ! -f "$TAR" ]]; then
  echo "package-appimage: building Linux tarball first ..."
  "$ROOT/scripts/package-xfsr.sh"
fi

echo "packaging AppImage $APPIMAGE_NAME ..."

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr"

# Unpack versioned tarball contents into AppDir/usr
STAGE_NAME="edgescale-${VERSION}-linux-${ARCH}"
TMP="$OUT_DIR/.appdir/extract"
rm -rf "$TMP"
mkdir -p "$TMP"
tar -C "$TMP" -xzf "$TAR"
# Tarball root is edgescale-VER-linux-ARCH/
cp -a "$TMP/$STAGE_NAME/." "$APPDIR/usr/"
rm -rf "$TMP"

# Layer JSON library_path already points at ../../../lib/libVkLayer_XFSR.so
# relative to share/vulkan/implicit_layer.d — correct for AppDir/usr layout.

# AppRun: portable launcher (no ~/.local install required)
cat > "$APPDIR/AppRun" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# appimagetool sets APPDIR when running the mounted image; fall back for unpacked AppDir.
export APPDIR="${APPDIR:-$HERE}"
PREFIX="$APPDIR/usr"

export PATH="$PREFIX/bin:${PATH:-}"
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Point edgerun / the Vulkan loader at the bundled layer + shaders.
export XFSR_LAYER_PATH="$PREFIX/share/vulkan/implicit_layer.d"
export EDGESCALE_LAYER_PATH="$XFSR_LAYER_PATH"
export XFSR_SHADER_DIR="$PREFIX/share/xfsr/shaders"
export EDGESCALE_SHADER_DIR="$XFSR_SHADER_DIR"
export VK_ADD_IMPLICIT_LAYER_PATH="$XFSR_LAYER_PATH"

# Steam / pressure-vessel: expose AppDir so Proton can see the layer + shaders.
export PRESSURE_VESSEL_FILESYSTEMS_RW="${APPDIR}${PRESSURE_VESSEL_FILESYSTEMS_RW:+:$PRESSURE_VESSEL_FILESYSTEMS_RW}"
export STEAM_COMPAT_MOUNTS="${APPDIR}${STEAM_COMPAT_MOUNTS:+:$STEAM_COMPAT_MOUNTS}"

exec "$PREFIX/bin/edgerun" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# Desktop entry (required by appimagetool)
cat > "$APPDIR/edgescale.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=EdgeScale
Comment=In-process GPU upscaling for Vulkan games
Exec=edgerun
Icon=edgescale
Categories=Game;Utility;
Terminal=true
NoDisplay=true
EOF

# Icon
if [[ -f "$ROOT/logo.png" ]]; then
  cp -f "$ROOT/logo.png" "$APPDIR/edgescale.png"
else
  # Minimal 1×1 PNG placeholder so appimagetool accepts the AppDir
  printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\x00\x00\x00\x00IEND\xaeB`\x82' \
    > "$APPDIR/edgescale.png"
fi

# Fetch appimagetool if needed
TOOL="${APPIMAGETOOL:-}"
if [[ -z "$TOOL" || ! -x "$TOOL" ]]; then
  TOOL_DIR="$OUT_DIR/.appdir/tools"
  mkdir -p "$TOOL_DIR"
  case "$ARCH" in
    x86_64) TOOL_ARCH=x86_64 ;;
    aarch64) TOOL_ARCH=aarch64 ;;
  esac
  TOOL="$TOOL_DIR/appimagetool-$TOOL_ARCH.AppImage"
  if [[ ! -x "$TOOL" ]]; then
    echo "Downloading appimagetool ..."
    URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${TOOL_ARCH}.AppImage"
    if ! curl -fsSL -o "$TOOL" "$URL"; then
      echo "package-appimage: failed to download appimagetool from $URL" >&2
      exit 1
    fi
    chmod +x "$TOOL"
  fi
fi

mkdir -p "$OUT_DIR"
# ARCH is required by appimagetool; disable FUSE extract when nested in CI
export ARCH
if [[ "${APPIMAGE_EXTRACT_AND_RUN:-}" == "1" ]] || [[ ! -e /dev/fuse ]]; then
  export APPIMAGE_EXTRACT_AND_RUN=1
fi

rm -f "$APPIMAGE_OUT"
(cd "$OUT_DIR" && "$TOOL" "$APPDIR" "$APPIMAGE_NAME")
chmod +x "$APPIMAGE_OUT"

# Convenience unversioned copy
cp -f "$APPIMAGE_OUT" "$OUT_DIR/EdgeScale-${ARCH}.AppImage"

ls -lh "$APPIMAGE_OUT" "$OUT_DIR/EdgeScale-${ARCH}.AppImage"
echo "OK $APPIMAGE_OUT"
