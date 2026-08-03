#!/usr/bin/env python3
"""Wire dumped BIOS/firmware from a local BIOS library into RetroArch's system dir."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from scriptutil import (
    REL_BIOS_ROOT,
    add_gaming_root_arg,
    env_path,
    resolve_gaming_path,
)


def link_file(src: Path, dest: Path) -> bool:
    if not src.exists():
        print(f"  skip (missing): {src}")
        return False
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.is_symlink() or dest.exists():
        dest.unlink()
    dest.symlink_to(src)
    print(f"  ok: {dest.name} -> {src}")
    return True


def main(argv: list[str] | None = None) -> int:
    home = Path.home()
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/link-system-bios.sh\n"
        "Expected layout under --gaming-root: BIOS FILES/PS1|PS2|NDS|3DS/…",
    )
    add_gaming_root_arg(parser)
    parser.add_argument(
        "--system-dir",
        type=Path,
        default=Path(
            os.environ.get(
                "ARCHSTREAMER_SYSTEM_DIR", str(home / ".config/retroarch/system")
            )
        ),
    )
    parser.add_argument(
        "--bios-root",
        type=Path,
        default=env_path("ARCHSTREAMER_BIOS_ROOT"),
        help="BIOS library root (default: <gaming-root>/BIOS FILES)",
    )
    args = parser.parse_args(argv)

    system_dir: Path = args.system_dir
    bios_root = resolve_gaming_path(
        gaming_root=args.gaming_root,
        relative=REL_BIOS_ROOT,
        override=args.bios_root,
        label="BIOS root (--bios-root)",
    )
    system_dir.mkdir(parents=True, exist_ok=True)

    print(f"System dir: {system_dir}")
    print(f"BIOS root:  {bios_root}")
    print()

    print("==> PS1 (swanstation / pcsx_rearmed)")
    ps1_ok = False
    if link_file(bios_root / "PS1/BIOS/SCPH1001.BIN", system_dir / "SCPH1001.BIN"):
        for alias in ("scph1001.bin", "scph5501.bin"):
            dest = system_dir / alias
            if dest.is_symlink() or dest.exists():
                dest.unlink()
            dest.symlink_to(system_dir / "SCPH1001.BIN")
        ps1_ok = True
    if (bios_root / "PS1/BIOS/SCPH5501.BIN").is_file():
        link_file(bios_root / "PS1/BIOS/SCPH5501.BIN", system_dir / "scph5501.bin")
    elif (bios_root / "PS1/BIOS/scph5501.bin").is_file():
        link_file(bios_root / "PS1/BIOS/scph5501.bin", system_dir / "scph5501.bin")

    print()
    print("==> PS2 (pcsx2 libretro -> system/pcsx2/bios)")
    ps2_dir = system_dir / "pcsx2/bios"
    ps2_dir.mkdir(parents=True, exist_ok=True)
    ps2_src = bios_root / "PS2/BIOS"
    ps2_ok = False
    if ps2_src.is_dir():
        for f in sorted(ps2_src.iterdir()):
            if not f.is_file():
                continue
            dest = ps2_dir / f.name
            if dest.is_symlink() or dest.exists():
                dest.unlink()
            dest.symlink_to(f)
            ps2_ok = True
        if ps2_ok:
            print(f"  ok: linked contents of {ps2_src} -> {ps2_dir}")
        else:
            print(f"  skip: no files in {ps2_src}")
    else:
        print(f"  skip: missing {ps2_src}")

    print()
    print("==> NDS (melonDS; optional but recommended)")
    nds_src = bios_root / "NDS/NDS BIOS & Firmware"
    nds_ok = False
    for name in ("bios7.bin", "bios9.bin", "firmware.bin"):
        if link_file(nds_src / name, system_dir / name):
            nds_ok = True
    if (nds_src / "BIOSGBA.ROM").is_file():
        link_file(nds_src / "BIOSGBA.ROM", system_dir / "BIOSGBA.ROM")

    print()
    print("==> 3DS (Citra; aes_keys + seeddb)")
    citra_ok = False
    aes_src: Path | None = None
    seed_src: Path | None = None
    for candidate in (
        bios_root / "3DS/aes_keys.txt",
        bios_root / "3DS/BIOS/aes_keys.txt",
    ):
        if candidate.is_file():
            aes_src = candidate
            break
    for candidate in (
        bios_root / "3DS/seeddb.bin",
        bios_root / "3DS/BIOS/seeddb.bin",
    ):
        if candidate.is_file():
            seed_src = candidate
            break

    xdg = os.environ.get("XDG_CONFIG_HOME", str(home / ".config"))
    citra_user_sysdata = (
        Path(xdg) / "retroarch/saves/Citra/Citra/sysdata"
    )
    (system_dir / "sysdata").mkdir(parents=True, exist_ok=True)
    citra_user_sysdata.mkdir(parents=True, exist_ok=True)

    if aes_src is not None:
        if link_file(aes_src, system_dir / "aes_keys.txt"):
            citra_ok = True
        for dest in (
            system_dir / "sysdata/aes_keys.txt",
            citra_user_sysdata / "aes_keys.txt",
        ):
            if dest.is_symlink() or dest.exists():
                dest.unlink()
            dest.symlink_to(aes_src)
        print("  ok: Citra user sysdata aes_keys.txt")
    if seed_src is not None:
        if link_file(seed_src, system_dir / "seeddb.bin"):
            citra_ok = True
        for dest in (
            system_dir / "sysdata/seeddb.bin",
            citra_user_sysdata / "seeddb.bin",
        ):
            if dest.is_symlink() or dest.exists():
                dest.unlink()
            dest.symlink_to(seed_src)
        print("  ok: Citra user sysdata seeddb.bin")
    if not citra_ok:
        print(f"  skip: no aes_keys.txt / seeddb.bin under {bios_root / '3DS'}")

    print()
    print("==> Notes (no auto-link)")
    print(
        f"""  PSP:    PPSSPP uses HLE — real PSP BIOS not required. Assets live under
          {system_dir}/PPSSPP (already present if the core has run once).
  NDS:    melonDS is Nintendo DS only — it cannot run 3DS titles (use Citra).
  N64:    No BIOS required for cartridge ROMs.
  GB/GBC/GBA/SNES: No BIOS required (GBA boot logo BIOS optional).
  GC/Wii: Dolphin Sys data under {system_dir}/dolphin-emu (no console BIOS dump).
  Switch: Uses standalone Yuzu keys (prod.keys), not RetroArch system/.
"""
    )

    print()
    print("Summary:")
    print(f"  PS1: {'linked' if ps1_ok else 'MISSING'}")
    print(f"  PS2: {'linked' if ps2_ok else 'MISSING'}")
    print(
        f"  NDS: {'linked' if nds_ok else 'missing (melonDS freeBIOS still works)'}"
    )
    print(
        f"  3DS: {'linked' if citra_ok else 'missing aes_keys/seeddb (encrypted dumps will fail)'}"
    )
    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
