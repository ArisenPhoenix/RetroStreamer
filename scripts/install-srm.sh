#!/usr/bin/env bash
# Download/update Steam ROM Manager AppImage.
set -euo pipefail

DEST_DIR="${ARCHSTREAMER_SRM_DIR:-/mnt/Internal_SSD/Gaming/tools/srm}"
DEST="${DEST_DIR}/Steam-ROM-Manager.AppImage"
VERSION="${ARCHSTREAMER_SRM_VERSION:-v2.5.43}"
URL="https://github.com/SteamGridDB/steam-rom-manager/releases/download/${VERSION}/Steam-ROM-Manager-${VERSION#v}.AppImage"

mkdir -p "${DEST_DIR}"
echo "Downloading ${URL}"
curl -L --fail -o "${DEST}.partial" "${URL}"
mv "${DEST}.partial" "${DEST}"
chmod +x "${DEST}"
ls -lh "${DEST}"
echo "Launch with: ./scripts/launch-srm.sh"
