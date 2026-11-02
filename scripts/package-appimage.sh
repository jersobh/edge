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

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  echo "package-appimage: building Linux package first ..."
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

# AppRun: portable launcher and offline installer/uninstaller
cat > "$APPDIR/AppRun" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# appimagetool sets APPDIR when running the mounted image; fall back for unpacked AppDir.
export APPDIR="${APPDIR:-$HERE}"
PREFIX="$APPDIR/usr"

# Handle --install
if [[ "${1:-}" == "--install" ]]; then
  TARGET="${2:-${HOME}/.local}"
  BIN_DIR="${TARGET}/bin"
  LIB_DIR="${TARGET}/lib"
  LAYER_DIR="${TARGET}/share/vulkan/implicit_layer.d"
  SHADER_DIR="${TARGET}/share/edgescale/shaders"
  LEGACY_SHADER_DIR="${TARGET}/share/xfsr/shaders"

  echo ">>> Installing EdgeScale into ${TARGET} ..."
  mkdir -p "$BIN_DIR" "$LIB_DIR" "$LAYER_DIR" "$SHADER_DIR" "$LEGACY_SHADER_DIR"

  cp -f "$PREFIX/bin/edgerun" "$BIN_DIR/edgerun"
  chmod 755 "$BIN_DIR/edgerun"
  ln -sf edgerun "$BIN_DIR/xfsr-run"

  if [[ -f "$PREFIX/bin/xfsr-auto-tune" ]]; then
    cp -f "$PREFIX/bin/xfsr-auto-tune" "$BIN_DIR/xfsr-auto-tune"
    chmod 755 "$BIN_DIR/xfsr-auto-tune"
    ln -sf xfsr-auto-tune "$BIN_DIR/edgescale-auto-tune"
  fi
  if [[ -f "$PREFIX/bin/xfsr-gamescope-focus" ]]; then
    cp -f "$PREFIX/bin/xfsr-gamescope-focus" "$BIN_DIR/xfsr-gamescope-focus"
    chmod 755 "$BIN_DIR/xfsr-gamescope-focus"
    ln -sf xfsr-gamescope-focus "$BIN_DIR/edgescale-gamescope-focus"
  fi

  cp -f "$PREFIX/lib/libVkLayer_XFSR.so" "$LIB_DIR/libVkLayer_XFSR.so"
  chmod 755 "$LIB_DIR/libVkLayer_XFSR.so"
  if [[ -f "$PREFIX/lib/libxfsr_gl_shim.so" ]]; then
    cp -f "$PREFIX/lib/libxfsr_gl_shim.so" "$LIB_DIR/libxfsr_gl_shim.so"
    chmod 755 "$LIB_DIR/libxfsr_gl_shim.so"
  fi

  if [[ -d "$PREFIX/share/xfsr/shaders" ]]; then
    cp -a "$PREFIX/share/xfsr/shaders/." "$SHADER_DIR/"
    cp -a "$PREFIX/share/xfsr/shaders/." "$LEGACY_SHADER_DIR/"
  fi

  CFG_DIR="${XDG_CONFIG_HOME:-${HOME}/.config}/edgescale"
  LEGACY_CFG="${XDG_CONFIG_HOME:-${HOME}/.config}/xfsr"
  mkdir -p "$CFG_DIR" "$LEGACY_CFG"
  if [[ -f "$PREFIX/share/xfsr/profiles.example.conf" ]]; then
    mkdir -p "${TARGET}/share/edgescale"
    cp -f "$PREFIX/share/xfsr/profiles.example.conf" "${TARGET}/share/edgescale/profiles.example.conf"
    if [[ ! -f "$CFG_DIR/profiles.conf" ]]; then
      cp -f "$PREFIX/share/xfsr/profiles.example.conf" "$CFG_DIR/profiles.conf"
    fi
    if [[ ! -f "$LEGACY_CFG/profiles.conf" ]]; then
      ln -sf "$CFG_DIR/profiles.conf" "$LEGACY_CFG/profiles.conf" 2>/dev/null || true
    fi
  fi

  SO_PATH="$(cd "$LIB_DIR" && pwd)/libVkLayer_XFSR.so"
  for jname in VkLayer_EdgeScale.json VkLayer_XFSR.json; do
    JSRC="$PREFIX/share/vulkan/implicit_layer.d/$jname"
    JDST="$LAYER_DIR/$jname"
    if [[ -f "$JSRC" ]]; then
      sed "s|\"library_path\"[[:space:]]*:[[:space:]]*\"[^\"]*\"|\"library_path\": \"$SO_PATH\"|" "$JSRC" > "$JDST"
    fi
  done

  mkdir -p "${TARGET}/share/applications" "${TARGET}/share/icons/hicolor/256x256/apps"
  if [[ -f "$APPDIR/edgescale.png" ]]; then
    cp -f "$APPDIR/edgescale.png" "${TARGET}/share/icons/hicolor/256x256/apps/edgescale.png"
  fi
  cat > "${TARGET}/share/applications/edgescale.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=EdgeScale
Comment=In-process GPU upscaling for Vulkan games
Exec=edgerun
Icon=edgescale
Categories=Game;Utility;
Terminal=true
NoDisplay=true
DESK

  echo
  echo "=================================================="
  echo "  EdgeScale installed successfully!"
  echo "  Installed to: ${TARGET}"
  echo "=================================================="
  echo "Runner:   ${BIN_DIR}/edgerun"
  echo "Layer:    ${LAYER_DIR}/VkLayer_EdgeScale.json"
  echo
  echo "Steam launch option:"
  echo "  edgerun --preset performance %command%"
  echo
  exit 0
fi

# Handle --uninstall
if [[ "${1:-}" == "--uninstall" ]]; then
  TARGET="${2:-${HOME}/.local}"
  echo ">>> Uninstalling EdgeScale from ${TARGET} ..."
  rm -f "${TARGET}/bin/edgerun" "${TARGET}/bin/xfsr-run" \
        "${TARGET}/bin/xfsr-auto-tune" "${TARGET}/bin/edgescale-auto-tune" \
        "${TARGET}/bin/xfsr-gamescope-focus" "${TARGET}/bin/edgescale-gamescope-focus"
  rm -f "${TARGET}/lib/libVkLayer_XFSR.so" "${TARGET}/lib/libxfsr_gl_shim.so"
  rm -f "${TARGET}/share/vulkan/implicit_layer.d/VkLayer_EdgeScale.json" \
        "${TARGET}/share/vulkan/implicit_layer.d/VkLayer_XFSR.json"
  rm -rf "${TARGET}/share/edgescale" "${TARGET}/share/xfsr"
  rm -f "${TARGET}/share/applications/edgescale.desktop"
  rm -f "${TARGET}/share/icons/hicolor/256x256/apps/edgescale.png"
  rmdir "${TARGET}/bin" "${TARGET}/lib" "${TARGET}/share/vulkan/implicit_layer.d" "${TARGET}/share/vulkan" 2>/dev/null || true
  rmdir "${TARGET}/share/applications" "${TARGET}/share/icons/hicolor/256x256/apps" "${TARGET}/share/icons/hicolor/256x256" "${TARGET}/share/icons/hicolor" "${TARGET}/share/icons" 2>/dev/null || true
  rmdir "${TARGET}/share" "${TARGET}" 2>/dev/null || true
  echo "EdgeScale uninstalled from ${TARGET}."
  exit 0
fi

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
