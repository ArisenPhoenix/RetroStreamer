#!/usr/bin/env bash
# Fetch CHD-capable PS1 libretro cores into ~/.config/retroarch/cores
# (same directory ra.py / ArchStreamer search before distro Beetle packages).
#
# Usage:
#   ./scripts/fetch-ps1-cores.sh
set -euo pipefail

CORE_DIR="${ARCHSTREAMER_CORE_DIR:-$HOME/.config/retroarch/cores}"
SYSTEM_DIR="${ARCHSTREAMER_SYSTEM_DIR:-$HOME/.config/retroarch/system}"
ARCH="${ARCHSTREAMER_LIBRETRO_ARCH:-x86_64}"
BASE="https://buildbot.libretro.com/nightly/linux/${ARCH}/latest"

mkdir -p "$CORE_DIR" "$SYSTEM_DIR"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fetch_core() {
  local name="$1"
  local zip="${name}_libretro.so.zip"
  echo "==> ${name}"
  curl -fsSL -o "${TMP}/${zip}" "${BASE}/${zip}"
  unzip -o "${TMP}/${zip}" -d "$CORE_DIR"
}

fetch_core pcsx_rearmed
fetch_core swanstation

if [[ -n "${ARCHSTREAMER_PS1_BIOS:-}" && -f "${ARCHSTREAMER_PS1_BIOS}" ]]; then
  echo "==> Installing BIOS from ${ARCHSTREAMER_PS1_BIOS}"
  cp -f "${ARCHSTREAMER_PS1_BIOS}" "${SYSTEM_DIR}/scph1001.bin"
  cp -f "${ARCHSTREAMER_PS1_BIOS}" "${SYSTEM_DIR}/SCPH1001.BIN"
  cp -f "${ARCHSTREAMER_PS1_BIOS}" "${SYSTEM_DIR}/scph5501.bin"
elif [[ ! -f "${SYSTEM_DIR}/scph5501.bin" && ! -f "${SYSTEM_DIR}/scph1001.bin" ]]; then
  echo "Note: no PS1 BIOS in ${SYSTEM_DIR}."
  echo "Set ARCHSTREAMER_PS1_BIOS=/path/to/SCPH1001.BIN and re-run, or copy BIOS there."
fi

echo
echo "Installed:"
ls -la "${CORE_DIR}/pcsx_rearmed_libretro.so" "${CORE_DIR}/swanstation_libretro.so"
echo "System dir: ${SYSTEM_DIR}"
