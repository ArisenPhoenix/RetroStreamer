#!/usr/bin/env python3
"""Linux GStreamer runtime for ArchStreamer client receive + host capture/encode."""

from __future__ import annotations

import argparse
import sys

from scriptutil import require_linux, run

PACKAGES = [
    "gstreamer1.0-tools",
    "gstreamer1.0-plugins-base",
    "gstreamer1.0-plugins-good",
    "gstreamer1.0-plugins-ugly",
    "gstreamer1.0-libav",
    "gstreamer1.0-x",
    "gstreamer1.0-pulseaudio",
    "gstreamer1.0-pipewire",
    "gstreamer1.0-plugins-bad",
]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/install_gst.sh",
    )
    parser.parse_args(argv)
    require_linux()

    run(["sudo", "apt", "update"])
    run(["sudo", "apt", "install", "-y", *PACKAGES])
    # Optional NVIDIA encode (host): nvh264enc lives in bad/nvcodec when the driver is present.
    return 0


if __name__ == "__main__":
    sys.exit(main())
