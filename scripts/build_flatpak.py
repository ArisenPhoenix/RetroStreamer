#!/usr/bin/env python3
"""Build a local Flatpak of ArchStreamer (good for Bazzite / immutable hosts)."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from scriptutil import eprint, repo_root, require_cmd, run


def main(argv: list[str] | None = None) -> int:
    root = repo_root()
    default_build = Path(
        os.environ.get("ARCHSTREAMER_FLATPAK_BUILD_DIR", str(root / "build-flatpak"))
    )
    default_repo = Path(
        os.environ.get("ARCHSTREAMER_FLATPAK_REPO_DIR", str(default_build / "repo"))
    )
    default_bundle = Path(
        os.environ.get(
            "ARCHSTREAMER_FLATPAK_BUNDLE", str(default_build / "ArchStreamer.flatpak")
        )
    )

    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/build-flatpak.sh",
    )
    parser.add_argument("--build-dir", type=Path, default=default_build)
    parser.add_argument("--repo-dir", type=Path, default=default_repo)
    parser.add_argument("--bundle", type=Path, default=default_bundle)
    args = parser.parse_args(argv)

    require_cmd("flatpak")
    try:
        require_cmd("flatpak-builder")
    except SystemExit:
        eprint("flatpak-builder is required.")
        eprint("  Fedora/Bazzite: sudo rpm-ostree install flatpak-builder   # then reboot")
        eprint("  or use a Fedora distrobox and install flatpak-builder there.")
        return 1

    manifest = root / "deploy/flatpak/io.github.ArisenPhoenix.ArchStreamer.yml"
    build_dir: Path = args.build_dir
    repo_dir: Path = args.repo_dir
    bundle: Path = args.bundle

    run(
        [
            "flatpak",
            "remote-add",
            "--if-not-exists",
            "--user",
            "flathub",
            "https://dl.flathub.org/repo/flathub.flatpakrepo",
        ]
    )
    run(
        [
            "flatpak",
            "install",
            "-y",
            "--user",
            "flathub",
            "org.kde.Platform//6.9",
            "org.kde.Sdk//6.9",
        ]
    )

    build_dir.mkdir(parents=True, exist_ok=True)
    run(
        [
            "flatpak-builder",
            "--force-clean",
            "--user",
            "--install-deps-from=flathub",
            f"--repo={repo_dir}",
            build_dir / "build",
            manifest,
        ]
    )
    run(
        [
            "flatpak",
            "build-bundle",
            repo_dir,
            bundle,
            "io.github.ArisenPhoenix.ArchStreamer",
        ]
    )

    print()
    print(f"Built bundle: {bundle}")
    print("Install on this or another machine with:")
    print(f"  flatpak install --user {bundle}")
    print("Run with:")
    print("  flatpak run io.github.ArisenPhoenix.ArchStreamer")
    return 0


if __name__ == "__main__":
    sys.exit(main())
