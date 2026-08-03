#!/usr/bin/env bash
# Launch Steam ROM Manager with ArchStreamer art defaults.
#
# Usage:
#   ARCHSTREAMER_GAMING_ROOT=/path/to/Gaming ./scripts/launch-srm.sh
#   ./scripts/launch-srm.sh /path/to/Gaming
set -euo pipefail

GAMING_ROOT="${ARCHSTREAMER_GAMING_ROOT:-}"
# Positional Gaming root before optional --sandbox / AppImage args
if [[ $# -ge 1 && "$1" != --* ]]; then
  GAMING_ROOT="$1"
  shift
fi

SRM_APPIMAGE="${ARCHSTREAMER_SRM_APPIMAGE:-}"
ART_ROOT="${ARCHSTREAMER_ART_ROOT:-}"
ROMS_ROOT="${ARCHSTREAMER_ROMS_ROOT:-}"

if [[ -n "$GAMING_ROOT" ]]; then
  [[ -n "$SRM_APPIMAGE" ]] || SRM_APPIMAGE="$GAMING_ROOT/tools/srm/Steam-ROM-Manager.AppImage"
  [[ -n "$ART_ROOT" ]] || ART_ROOT="$GAMING_ROOT/ROMS/Art"
  [[ -n "$ROMS_ROOT" ]] || ROMS_ROOT="$GAMING_ROOT/ROMS/Games"
fi

if [[ -z "$SRM_APPIMAGE" || -z "$ART_ROOT" || -z "$ROMS_ROOT" ]]; then
  echo "Set ARCHSTREAMER_GAMING_ROOT (or SRM/ART/ROMS path env vars), or pass Gaming root as \$1." >&2
  exit 2
fi

if [[ ! -x "${SRM_APPIMAGE}" ]]; then
  echo "Steam ROM Manager AppImage not found at: ${SRM_APPIMAGE}" >&2
  echo "Download it with ARCHSTREAMER_GAMING_ROOT=… ./scripts/install-srm.sh" >&2
  exit 1
fi

mkdir -p \
  "${ART_ROOT}/default" \
  "${ART_ROOT}/poster" \
  "${ART_ROOT}/heroes" \
  "${ART_ROOT}/logos" \
  "${ART_ROOT}/icons" \
  "${ART_ROOT}/grids"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
"${ROOT}/scripts/ensure_srm_steam_layout.sh"

echo "Steam ROM Manager: ${SRM_APPIMAGE}"
echo "ROMs root:         ${ROMS_ROOT}"
echo "Art root:          ${ART_ROOT}"
echo
echo "In SRM Settings → Environment Variables, set:"
echo "  ROMs Directory:           ${ROMS_ROOT}"
echo "  Local Images Directory:   ${ART_ROOT}"
echo
echo "Recommended Local Artwork globs (per parser):"
echo "  poster: \${localImagesDir}/poster/\${title}.@(png|jpg|jpeg|webp)"
echo "  hero:   \${localImagesDir}/heroes/\${title}.@(png|jpg|jpeg|webp)"
echo "  logo:   \${localImagesDir}/logos/\${title}.@(png|jpg|jpeg|webp)"
echo "  icon:   \${localImagesDir}/icons/\${title}.@(png|jpg|jpeg|webp)"
echo
echo "Enable 'DRM Protect' / artwork backup on parsers so SGDB choices are cached locally."
echo "Then run: ARCHSTREAMER_GAMING_ROOT=… ./scripts/sync_srm_art_into_catalog.sh"
echo

# AppImage Electron sandbox is often broken without a setuid chrome-sandbox.
# Default to --no-sandbox for local desktop use; pass --sandbox to force it.
if [[ "${1:-}" == "--sandbox" ]]; then
  shift
  exec "${SRM_APPIMAGE}" "$@"
fi

exec "${SRM_APPIMAGE}" --no-sandbox "$@"
