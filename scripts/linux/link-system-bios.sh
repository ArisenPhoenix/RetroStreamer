#!/usr/bin/env bash
# Wire dumped BIOS/firmware from a local BIOS library into RetroArch's system dir
# (the same tree ArchStreamer points at via system_directory).
#
# Default source layout (override with ARCHSTREAMER_BIOS_ROOT):
#   $BIOS_ROOT/PS1/BIOS/SCPH*.BIN
#   $BIOS_ROOT/PS2/BIOS/SCPH-*.bin
#   $BIOS_ROOT/NDS/NDS BIOS & Firmware/{bios7,bios9,firmware}.bin
#
# Usage:
#   ./scripts/link-system-bios.sh
#   ARCHSTREAMER_BIOS_ROOT=/path/to/BIOS\ FILES ./scripts/link-system-bios.sh
set -euo pipefail

SYSTEM_DIR="${ARCHSTREAMER_SYSTEM_DIR:-$HOME/.config/retroarch/system}"
BIOS_ROOT="${ARCHSTREAMER_BIOS_ROOT:-<Gaming>/BIOS FILES}"

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
# Prefer a real SCPH-5501 if present alongside.
if [[ -f "$BIOS_ROOT/PS1/BIOS/SCPH5501.BIN" ]]; then
  link_file "$BIOS_ROOT/PS1/BIOS/SCPH5501.BIN" "$SYSTEM_DIR/scph5501.bin" || true
elif [[ -f "$BIOS_ROOT/PS1/BIOS/scph5501.bin" ]]; then
  link_file "$BIOS_ROOT/PS1/BIOS/scph5501.bin" "$SYSTEM_DIR/scph5501.bin" || true
fi

echo
echo "==> PS2 (pcsx2 libretro -> system/pcsx2/bios)"
ps2_dir="$SYSTEM_DIR/pcsx2/bios"
mkdir -p "$ps2_dir"
ps2_src="$BIOS_ROOT/PS2/BIOS"
ps2_ok=0
if [[ -d "$ps2_src" ]]; then
  shopt -s nullglob
  for f in "$ps2_src"/*; do
    [[ -f "$f" ]] || continue
    ln -sfn "$f" "$ps2_dir/$(basename "$f")"
    ps2_ok=1
  done
  shopt -u nullglob
  if [[ "$ps2_ok" -eq 1 ]]; then
    echo "  ok: linked contents of $ps2_src -> $ps2_dir"
  else
    echo "  skip: no files in $ps2_src"
  fi
else
  echo "  skip: missing $ps2_src"
fi

echo
echo "==> NDS (melonDS; optional but recommended)"
nds_src="$BIOS_ROOT/NDS/NDS BIOS & Firmware"
nds_ok=0
link_file "$nds_src/bios7.bin" "$SYSTEM_DIR/bios7.bin" && nds_ok=1 || true
link_file "$nds_src/bios9.bin" "$SYSTEM_DIR/bios9.bin" && nds_ok=1 || true
link_file "$nds_src/firmware.bin" "$SYSTEM_DIR/firmware.bin" && nds_ok=1 || true
[[ -f "$nds_src/BIOSGBA.ROM" ]] && link_file "$nds_src/BIOSGBA.ROM" "$SYSTEM_DIR/BIOSGBA.ROM" || true

echo
echo "==> 3DS (Citra; aes_keys + seeddb)"
citra_ok=0
aes_src=""
seed_src=""
for candidate in \
  "$BIOS_ROOT/3DS/aes_keys.txt" \
  "$BIOS_ROOT/3DS/BIOS/aes_keys.txt"
do
  [[ -f "$candidate" ]] && aes_src="$candidate" && break
done
for candidate in \
  "$BIOS_ROOT/3DS/seeddb.bin" \
  "$BIOS_ROOT/3DS/BIOS/seeddb.bin"
do
  [[ -f "$candidate" ]] && seed_src="$candidate" && break
done

# RetroArch system_directory (some builds) + Citra libretro user sysdata
# (libretro Citra reads ~/.config/retroarch/saves/Citra/Citra/sysdata/).
citra_user_sysdata="${XDG_CONFIG_HOME:-$HOME/.config}/retroarch/saves/Citra/Citra/sysdata"
mkdir -p "$SYSTEM_DIR/sysdata" "$citra_user_sysdata"

if [[ -n "$aes_src" ]]; then
  link_file "$aes_src" "$SYSTEM_DIR/aes_keys.txt" && citra_ok=1
  ln -sfn "$aes_src" "$SYSTEM_DIR/sysdata/aes_keys.txt"
  ln -sfn "$aes_src" "$citra_user_sysdata/aes_keys.txt"
  echo "  ok: Citra user sysdata aes_keys.txt"
fi
if [[ -n "$seed_src" ]]; then
  link_file "$seed_src" "$SYSTEM_DIR/seeddb.bin" && citra_ok=1
  ln -sfn "$seed_src" "$SYSTEM_DIR/sysdata/seeddb.bin"
  ln -sfn "$seed_src" "$citra_user_sysdata/seeddb.bin"
  echo "  ok: Citra user sysdata seeddb.bin"
fi
[[ "$citra_ok" -eq 0 ]] && echo "  skip: no aes_keys.txt / seeddb.bin under $BIOS_ROOT/3DS"

echo
echo "==> Notes (no auto-link)"
cat <<EOF
  PSP:    PPSSPP uses HLE — real PSP BIOS not required. Assets live under
          ${SYSTEM_DIR}/PPSSPP (already present if the core has run once).
  NDS:    melonDS is Nintendo DS only — it cannot run 3DS titles (use Citra).
  N64:    No BIOS required for cartridge ROMs.
  GB/GBC/GBA/SNES: No BIOS required (GBA boot logo BIOS optional).
  GC/Wii: Dolphin Sys data under ${SYSTEM_DIR}/dolphin-emu (no console BIOS dump).
  Switch: Uses standalone Yuzu keys (prod.keys), not RetroArch system/.
EOF

echo
echo "Summary:"
[[ "$ps1_ok" -eq 1 ]] && echo "  PS1: linked" || echo "  PS1: MISSING"
[[ "$ps2_ok" -eq 1 ]] && echo "  PS2: linked" || echo "  PS2: MISSING"
[[ "$nds_ok" -eq 1 ]] && echo "  NDS: linked" || echo "  NDS: missing (melonDS freeBIOS still works)"
[[ "$citra_ok" -eq 1 ]] && echo "  3DS: linked" || echo "  3DS: missing aes_keys/seeddb (encrypted dumps will fail)"
echo "Done."
