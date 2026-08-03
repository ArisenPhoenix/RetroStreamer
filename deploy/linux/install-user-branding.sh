#!/usr/bin/env bash
# Install ArchStreamer .desktop + hicolor icons for the current user so
# terminal / build-tree runs get the branding mark under GNOME/KDE/Wayland.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BR="$ROOT/branding"
APP_ID=io.github.ArisenPhoenix.ArchStreamer
ICON_NAME="$APP_ID"

BIN="${1:-}"
if [[ -z "$BIN" ]]; then
  if [[ -x "$ROOT/build/archstreamer_gui" ]]; then
    BIN="$ROOT/build/archstreamer_gui"
  elif command -v archstreamer_gui >/dev/null 2>&1; then
    BIN="$(command -v archstreamer_gui)"
  else
    echo "usage: $0 /path/to/archstreamer_gui" >&2
    exit 1
  fi
fi
BIN="$(readlink -f "$BIN")"

APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICON_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor"
mkdir -p "$APP_DIR"
mkdir -p "$ICON_ROOT/scalable/apps"
for size in 128 256 512; do
  mkdir -p "$ICON_ROOT/${size}x${size}/apps"
  install -m644 "$BR/archstreamer-icon-${size}.png" \
    "$ICON_ROOT/${size}x${size}/apps/${ICON_NAME}.png"
done
install -m644 "$BR/archstreamer-icon.svg" "$ICON_ROOT/scalable/apps/${ICON_NAME}.svg"

# Desktop entry: Exec uses the absolute binary; StartupWMClass matches Qt app name.
cat >"$APP_DIR/${APP_ID}.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ArchStreamer
Comment=Local/LAN RetroArch streaming host and client
Exec=${BIN}
Icon=${ICON_NAME}
Terminal=false
Categories=Game;Emulator;
StartupWMClass=ArchStreamer
EOF

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f -t "$ICON_ROOT" >/dev/null 2>&1 || true
fi

echo "Installed user desktop entry: $APP_DIR/${APP_ID}.desktop"
echo "  Exec=$BIN"
echo "  Icons under $ICON_ROOT"
echo "Re-launch ArchStreamer (quit fully first) to pick up the icon."
