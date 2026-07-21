#!/usr/bin/env bash
# Launch Steam ROM Manager with ArchStreamer art defaults.
set -euo pipefail

SRM_APPIMAGE="${ARCHSTREAMER_SRM_APPIMAGE:-/mnt/Internal_SSD/Gaming/tools/srm/Steam-ROM-Manager.AppImage}"
ART_ROOT="${ARCHSTREAMER_ART_ROOT:-/mnt/Internal_SSD/Gaming/ROMS/Art}"
ROMS_ROOT="${ARCHSTREAMER_ROMS_ROOT:-/mnt/Internal_SSD/Gaming/ROMS/Games}"

if [[ ! -x "${SRM_APPIMAGE}" ]]; then
  echo "Steam ROM Manager AppImage not found at: ${SRM_APPIMAGE}" >&2
  echo "Download it with scripts/install-srm.sh" >&2
  exit 1
fi

mkdir -p \
  "${ART_ROOT}/default" \
  "${ART_ROOT}/poster" \
  "${ART_ROOT}/heroes" \
  "${ART_ROOT}/logos" \
  "${ART_ROOT}/icons" \
  "${ART_ROOT}/grids"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
echo "Then run: ./scripts/sync_srm_art_into_catalog.sh"
echo

# AppImage Electron sandbox is often broken without a setuid chrome-sandbox.
# Default to --no-sandbox for local desktop use; pass --sandbox to force it.
if [[ "${1:-}" == "--sandbox" ]]; then
  shift
  exec "${SRM_APPIMAGE}" "$@"
fi

exec "${SRM_APPIMAGE}" --no-sandbox "$@"
