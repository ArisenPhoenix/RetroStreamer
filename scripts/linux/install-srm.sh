#!/usr/bin/env bash
# Download/update Steam ROM Manager AppImage.
#
# Usage:
#   ARCHSTREAMER_GAMING_ROOT=/path/to/Gaming ./scripts/install-srm.sh
#   ARCHSTREAMER_SRM_DIR=/path/to/tools/srm ./scripts/install-srm.sh
#   ./scripts/install-srm.sh /path/to/Gaming
set -euo pipefail

GAMING_ROOT="${ARCHSTREAMER_GAMING_ROOT:-${1:-}}"
DEST_DIR="${ARCHSTREAMER_SRM_DIR:-}"
if [[ -z "$DEST_DIR" ]]; then
  if [[ -z "$GAMING_ROOT" ]]; then
    echo "Set ARCHSTREAMER_GAMING_ROOT or ARCHSTREAMER_SRM_DIR, or pass Gaming root as \$1." >&2
    exit 2
  fi
  DEST_DIR="$GAMING_ROOT/tools/srm"
fi

DEST="${DEST_DIR}/Steam-ROM-Manager.AppImage"
VERSION="${ARCHSTREAMER_SRM_VERSION:-v2.5.43}"
URL="https://github.com/SteamGridDB/steam-rom-manager/releases/download/${VERSION}/Steam-ROM-Manager-${VERSION#v}.AppImage"

mkdir -p "${DEST_DIR}"
echo "Downloading ${URL}"
curl -L --fail -o "${DEST}.partial" "${URL}"
mv "${DEST}.partial" "${DEST}"
chmod +x "${DEST}"
ls -lh "${DEST}"
echo "Launch with: ARCHSTREAMER_GAMING_ROOT=… ./scripts/launch-srm.sh"
