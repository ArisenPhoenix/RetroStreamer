#!/usr/bin/env bash
# Wire dumped BIOS/firmware from a local BIOS library into RetroArch's system dir
# (the same tree ArchStreamer points at via system_directory).
#
# Expected source layout under BIOS root:
#   $BIOS_ROOT/PS1/BIOS/SCPH*.BIN
#   $BIOS_ROOT/PS2/BIOS/SCPH-*.bin
#   $BIOS_ROOT/NDS/NDS BIOS & Firmware/{bios7,bios9,firmware}.bin
#
# Usage:
#   ARCHSTREAMER_GAMING_ROOT=/path/to/Gaming ./scripts/link-system-bios.sh
#   ARCHSTREAMER_BIOS_ROOT=/path/to/BIOS\ FILES ./scripts/link-system-bios.sh
#   ./scripts/link-system-bios.sh /path/to/Gaming
set -euo pipefail

SYSTEM_DIR="${ARCHSTREAMER_SYSTEM_DIR:-$HOME/.config/retroarch/system}"
GAMING_ROOT="${ARCHSTREAMER_GAMING_ROOT:-${1:-}}"
BIOS_ROOT="${ARCHSTREAMER_BIOS_ROOT:-}"

if [[ -z "$BIOS_ROOT" ]]; then
  if [[ -z "$GAMING_ROOT" ]]; then
    echo "Set ARCHSTREAMER_GAMING_ROOT or ARCHSTREAMER_BIOS_ROOT, or pass Gaming root as \$1." >&2
    exit 2
  fi
  BIOS_ROOT="$GAMING_ROOT/BIOS FILES"
fi

mkdir -p "$SYSTEM_DIR"

link_file() {
  local src="$1" dest="$2"
  if [[ ! -e "$src" ]]; then
    echo "  skip (missing): $src"
    return 1
  fi
  mkdir -p "$(dirname "$dest")"
  ln -sfn "$src" "$dest"
  echo "  ok: $(basename "$dest") -> $src"
  return 0
}

echo "System dir: $SYSTEM_DIR"
echo "BIOS root:  $BIOS_ROOT"
echo

echo "==> PS1 (swanstation / pcsx_rearmed)"
ps1_ok=0
if link_file "$BIOS_ROOT/PS1/BIOS/SCPH1001.BIN" "$SYSTEM_DIR/SCPH1001.BIN"; then
  ln -sfn "$SYSTEM_DIR/SCPH1001.BIN" "$SYSTEM_DIR/scph1001.bin"
  # Many cores also look for the 5501 USA dump; same file works for HLE boot paths.
  ln -sfn "$SYSTEM_DIR/SCPH1001.BIN" "$SYSTEM_DIR/scph5501.bin"
  ps1_ok=1
fi
if [[ -f "$BIOS_ROOT/PS1/BIOS/SCPH5501.BIN" ]]; then
  link_file "$BIOS_ROOT/PS1/BIOS/SCPH5501.BIN" "$SYSTEM_DIR/scph5501.bin" || true
elif [[ -f "$BIOS_ROOT/PS1/BIOS/scph5501.bin" ]]; then
  link_file "$BIOS_ROOT/PS1/BIOS/scph5501.bin" "$SYSTEM_DIR/scph5501.bin" || true
fi

echo
echo "==> PS2 (pcsx2 libretro -> system/pcsx2/bios)"
PS2_DIR="$SYSTEM_DIR/pcsx2/bios"
mkdir -p "$PS2_DIR"
PS2_SRC="$BIOS_ROOT/PS2/BIOS"
ps2_ok=0
if [[ -d "$PS2_SRC" ]]; then
  shopt -s nullglob
  for f in "$PS2_SRC"/*; do
    [[ -f "$f" ]] || continue
    ln -sfn "$f" "$PS2_DIR/$(basename "$f")"
    ps2_ok=1
  done
  shopt -u nullglob
  if [[ $ps2_ok -eq 1 ]]; then
    echo "  ok: linked contents of $PS2_SRC -> $PS2_DIR"
  else
    echo "  skip: no files in $PS2_SRC"
  fi
else
  echo "  skip: missing $PS2_SRC"
fi

echo
echo "==> NDS (melonDS; optional but recommended)"
NDS_SRC="$BIOS_ROOT/NDS/NDS BIOS & Firmware"
nds_ok=0
for name in bios7.bin bios9.bin firmware.bin; do
  if link_file "$NDS_SRC/$name" "$SYSTEM_DIR/$name"; then
    nds_ok=1
  fi
done
if [[ -f "$NDS_SRC/BIOSGBA.ROM" ]]; then
  link_file "$NDS_SRC/BIOSGBA.ROM" "$SYSTEM_DIR/BIOSGBA.ROM" || true
fi

echo
echo "==> 3DS (Citra; aes_keys + seeddb)"
citra_ok=0
AES_SRC=""
SEED_SRC=""
for candidate in "$BIOS_ROOT/3DS/aes_keys.txt" "$BIOS_ROOT/3DS/BIOS/aes_keys.txt"; do
  if [[ -f "$candidate" ]]; then
    AES_SRC="$candidate"
    break
  fi
done
for candidate in "$BIOS_ROOT/3DS/seeddb.bin" "$BIOS_ROOT/3DS/BIOS/seeddb.bin"; do
  if [[ -f "$candidate" ]]; then
    SEED_SRC="$candidate"
    break
  fi
done

XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
CITRA_USER_SYSDATA="$XDG_CONFIG_HOME/retroarch/saves/Citra/Citra/sysdata"
mkdir -p "$SYSTEM_DIR/sysdata" "$CITRA_USER_SYSDATA"

if [[ -n "$AES_SRC" ]]; then
  if link_file "$AES_SRC" "$SYSTEM_DIR/aes_keys.txt"; then
    citra_ok=1
  fi
  ln -sfn "$AES_SRC" "$SYSTEM_DIR/sysdata/aes_keys.txt"
  ln -sfn "$AES_SRC" "$CITRA_USER_SYSDATA/aes_keys.txt"
  echo "  ok: Citra user sysdata aes_keys.txt"
fi
if [[ -n "$SEED_SRC" ]]; then
  if link_file "$SEED_SRC" "$SYSTEM_DIR/seeddb.bin"; then
    citra_ok=1
  fi
  ln -sfn "$SEED_SRC" "$SYSTEM_DIR/sysdata/seeddb.bin"
  ln -sfn "$SEED_SRC" "$CITRA_USER_SYSDATA/seeddb.bin"
  echo "  ok: Citra user sysdata seeddb.bin"
fi
if [[ $citra_ok -eq 0 ]]; then
  echo "  skip: no aes_keys.txt / seeddb.bin under $BIOS_ROOT/3DS"
fi

echo
echo "==> Notes (no auto-link)"
cat <<EOF
  PSP:    PPSSPP uses HLE — real PSP BIOS not required. Assets live under
          $SYSTEM_DIR/PPSSPP (already present if the core has run once).
  NDS:    melonDS is Nintendo DS only — it cannot run 3DS titles (use Citra).
  N64:    No BIOS required for cartridge ROMs.
  GB/GBC/GBA/SNES: No BIOS required (GBA boot logo BIOS optional).
  GC/Wii: Dolphin Sys data under $SYSTEM_DIR/dolphin-emu (no console BIOS dump).
  Switch: Uses standalone Yuzu keys (prod.keys), not RetroArch system/.
EOF

echo
echo "Summary:"
echo "  PS1: $([[ $ps1_ok -eq 1 ]] && echo linked || echo MISSING)"
echo "  PS2: $([[ $ps2_ok -eq 1 ]] && echo linked || echo MISSING)"
echo "  NDS: $([[ $nds_ok -eq 1 ]] && echo linked || echo 'missing (melonDS freeBIOS still works)')"
echo "  3DS: $([[ $citra_ok -eq 1 ]] && echo linked || echo 'missing aes_keys/seeddb (encrypted dumps will fail)')"
echo "Done."
