#!/usr/bin/env bash
# Build a local Flatpak of ArchStreamer (good for Bazzite / immutable hosts).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${ROOT}/deploy/flatpak/io.github.ArisenPhoenix.ArchStreamer.yml"
BUILD_DIR="${ARCHSTREAMER_FLATPAK_BUILD_DIR:-${ROOT}/build-flatpak}"
REPO_DIR="${ARCHSTREAMER_FLATPAK_REPO_DIR:-${BUILD_DIR}/repo}"
BUNDLE="${ARCHSTREAMER_FLATPAK_BUNDLE:-${BUILD_DIR}/ArchStreamer.flatpak}"

if ! command -v flatpak >/dev/null 2>&1; then
  echo "flatpak is required." >&2
  exit 1
fi
if ! command -v flatpak-builder >/dev/null 2>&1; then
  echo "flatpak-builder is required." >&2
  echo "  Fedora/Bazzite: sudo rpm-ostree install flatpak-builder   # then reboot" >&2
  echo "  or use a Fedora distrobox and install flatpak-builder there." >&2
  exit 1
fi

flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install -y --user flathub org.kde.Platform//6.8 org.kde.Sdk//6.8

mkdir -p "${BUILD_DIR}"
flatpak-builder --force-clean --user --install-deps-from=flathub \
  --repo="${REPO_DIR}" \
  "${BUILD_DIR}/build" \
  "${MANIFEST}"

flatpak build-bundle "${REPO_DIR}" "${BUNDLE}" io.github.ArisenPhoenix.ArchStreamer

echo
echo "Built bundle: ${BUNDLE}"
echo "Install on this or another machine with:"
echo "  flatpak install --user ${BUNDLE}"
echo "Run with:"
echo "  flatpak run io.github.ArisenPhoenix.ArchStreamer"
