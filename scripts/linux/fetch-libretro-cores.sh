#!/usr/bin/env bash
# Fetch libretro cores from buildbot into ~/.config/retroarch/cores
# (same search path as ArchStreamer / RetroArchSysBin/ra.py).
#
# Default: cores for systems that typically need buildbot packages beyond apt.
# Override list: ARCHSTREAMER_CORES="melonds pcsx2 dolphin" ./scripts/fetch-libretro-cores.sh
#
# Usage:
#   ./scripts/fetch-libretro-cores.sh
#   ARCHSTREAMER_GAMING_ROOT=/path/to/Gaming ./scripts/fetch-libretro-cores.sh --catalog-systems
set -euo pipefail

CORE_DIR="${ARCHSTREAMER_CORE_DIR:-$HOME/.config/retroarch/cores}"
SYSTEM_DIR="${ARCHSTREAMER_SYSTEM_DIR:-$HOME/.config/retroarch/system}"
ARCH="${ARCHSTREAMER_LIBRETRO_ARCH:-x86_64}"
BASE="https://buildbot.libretro.com/nightly/linux/${ARCH}/latest"
GAMING_ROOT="${ARCHSTREAMER_GAMING_ROOT:-}"
ROM_ROOT="${ARCHSTREAMER_ROM_ROOT:-}"
if [[ -z "$ROM_ROOT" && -n "$GAMING_ROOT" ]]; then
  ROM_ROOT="$GAMING_ROOT/ROMS/Games"
fi

# Preferred cores aligned with ra.py / ArchStreamer (buildbot names).
# Skip standalone-only (yuzu) and apt-already-covered when present.
DEFAULT_CORES=(
  # Handheld / cart
  melonds
  sameboy
  vbam
  # Consoles present under typical ROM trees
  mupen64plus_next
  parallel_n64
  pcsx_rearmed
  swanstation
  pcsx2
  ppsspp
  dolphin
  citra
  nestopia
  fceumm
  bsnes
  snes9x2010
  genesis_plus_gx
  picodrive
  mednafen_pce_fast
)

mkdir -p "$CORE_DIR" "$SYSTEM_DIR"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cores_for_rom_tree() {
  local wanted=()
  [[ -d "$ROM_ROOT/GB" || -d "$ROM_ROOT/GBC" ]] && wanted+=(sameboy)
  [[ -d "$ROM_ROOT/GBA" ]] && wanted+=(vbam)
  [[ -d "$ROM_ROOT/NDS" || -d "$ROM_ROOT/DS" ]] && wanted+=(melonds)
  [[ -d "$ROM_ROOT/N64" ]] && wanted+=(mupen64plus_next parallel_n64)
  [[ -d "$ROM_ROOT/PS1" || -d "$ROM_ROOT/PSX" ]] && wanted+=(pcsx_rearmed swanstation)
  [[ -d "$ROM_ROOT/PS2" ]] && wanted+=(pcsx2)
  [[ -d "$ROM_ROOT/PSP" ]] && wanted+=(ppsspp)
  [[ -d "$ROM_ROOT/GameCube" || -d "$ROM_ROOT/GC" || -d "$ROM_ROOT/Wii" ]] && wanted+=(dolphin)
  [[ -d "$ROM_ROOT/3DS" ]] && wanted+=(citra)
  [[ -d "$ROM_ROOT/SNES" || -d "$ROM_ROOT/SFC" ]] && wanted+=(bsnes snes9x2010)
  [[ -d "$ROM_ROOT/NES" || -d "$ROM_ROOT/Famicom" ]] && wanted+=(nestopia fceumm)
  [[ -d "$ROM_ROOT/Genesis" || -d "$ROM_ROOT/MegaDrive" || -d "$ROM_ROOT/SMS" ]] && wanted+=(genesis_plus_gx picodrive)
  [[ -d "$ROM_ROOT/PCE" || -d "$ROM_ROOT/TG16" ]] && wanted+=(mednafen_pce_fast)
  # dedupe
  printf '%s\n' "${wanted[@]}" | awk 'NF && !seen[$0]++'
}

CORES=()
if [[ "${1:-}" == "--catalog-systems" || "${1:-}" == "--rom-tree" ]]; then
  if [[ -z "$ROM_ROOT" ]]; then
    echo "--catalog-systems needs ARCHSTREAMER_GAMING_ROOT or ARCHSTREAMER_ROM_ROOT." >&2
    exit 2
  fi
  mapfile -t CORES < <(cores_for_rom_tree)
elif [[ -n "${ARCHSTREAMER_CORES:-}" ]]; then
  # shellcheck disable=SC2206
  CORES=($ARCHSTREAMER_CORES)
else
  CORES=("${DEFAULT_CORES[@]}")
fi

if [[ "${#CORES[@]}" -eq 0 ]]; then
  echo "No cores selected." >&2
  exit 1
fi

fetch_core() {
  local name="$1"
  local zip="${name}_libretro.so.zip"
  local so="${name}_libretro.so"
  if [[ -f "${CORE_DIR}/${so}" ]]; then
    echo "==> ${name} (already present, refreshing)"
  else
    echo "==> ${name}"
  fi
  if ! curl -fsSL -o "${TMP}/${zip}" "${BASE}/${zip}"; then
    echo "    skip: not on buildbot (${zip})" >&2
    return 0
  fi
  unzip -o "${TMP}/${zip}" -d "$CORE_DIR" >/dev/null
  if [[ -f "${CORE_DIR}/${so}" ]]; then
    chmod +x "${CORE_DIR}/${so}"
    ls -lh "${CORE_DIR}/${so}"
  else
    echo "    warning: zip extracted but ${so} not found" >&2
    unzip -l "${TMP}/${zip}" | head -20
  fi
}

echo "Core dir: ${CORE_DIR}"
echo "Fetching: ${CORES[*]}"
echo

for core in "${CORES[@]}"; do
  fetch_core "$core"
done

echo
echo "Done. Installed cores in ${CORE_DIR}:"
ls -1 "${CORE_DIR}"/*_libretro.so 2>/dev/null | xargs -r -n1 basename | sort

cat <<EOF

Notes:
  - Restart ArchStreamer host to rescan the catalog (N64/PS2/PSP/etc. need these cores).
  - BIOS / firmware still required for some systems under ${SYSTEM_DIR}:
      PS1:  scph5501.bin / scph1001.bin
      PS2:  SCPH-*.bin under ${SYSTEM_DIR}/pcsx2/bios  (PCSX2 libretro)
      NDS:  bios7.bin + bios9.bin + firmware.bin (optional; melonDS freeBIOS works)
      PSP:  no real BIOS (PPSSPP HLE); assets under ${SYSTEM_DIR}/PPSSPP
      3DS:  aes_keys.txt + seeddb.bin (Citra; also copied into saves/Citra/.../sysdata)
            melonDS cannot run 3DS — it is Nintendo DS only
      Switch: not shipped as a libretro core here (use standalone / ra.py Yuzu fallback)
  - Wire dumps from your BIOS library:
      ./scripts/link-system-bios.sh
      ARCHSTREAMER_PS1_BIOS=/path/to/SCPH1001.BIN ./scripts/fetch-ps1-cores.sh
EOF
