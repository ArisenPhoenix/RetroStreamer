#!/usr/bin/env python3
"""Apply ArchStreamer LAN firewall allowances. Requires root."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

from scriptutil import eprint, run, which


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/apply-firewall.sh",
    )
    parser.add_argument(
        "--control-tcp",
        type=int,
        default=int(os.environ.get("ARCHSTREAMER_CONTROL_PORT", "45555")),
    )
    parser.add_argument(
        "--input-udp",
        type=int,
        default=int(os.environ.get("ARCHSTREAMER_INPUT_PORT", "45454")),
    )
    parser.add_argument(
        "--discovery-udp",
        type=int,
        default=int(os.environ.get("ARCHSTREAMER_DISCOVERY_PORT", "45550")),
    )
    parser.add_argument(
        "--video-udp-start",
        type=int,
        default=int(os.environ.get("ARCHSTREAMER_VIDEO_PORT", "5004")),
    )
    parser.add_argument(
        "--audio-udp-start",
        type=int,
        default=int(os.environ.get("ARCHSTREAMER_AUDIO_PORT", "6004")),
    )
    parser.add_argument(
        "--media-span",
        type=int,
        default=int(os.environ.get("ARCHSTREAMER_MEDIA_PORT_SPAN", "8")),
    )
    args = parser.parse_args(argv)

    if os.geteuid() != 0:
        eprint(f"Run with sudo: sudo {sys.argv[0]}")
        return 1

    control_tcp = args.control_tcp
    input_udp = args.input_udp
    discovery_udp = args.discovery_udp
    video_start = args.video_udp_start
    audio_start = args.audio_udp_start
    media_span = args.media_span
    video_end = video_start + media_span - 1
    audio_end = audio_start + media_span - 1

    print("ArchStreamer firewall allowances:")
    print(f"  TCP  {control_tcp}            (session control — host)")
    print(f"  UDP  {input_udp}              (controller input — host)")
    print(f"  UDP  {discovery_udp}          (LAN host discovery — host)")
    print(f"  UDP  {video_start}-{video_end}   (RTP video — CLIENT inbound)")
    print(f"  UDP  {audio_start}-{audio_end}   (RTP audio — CLIENT inbound)")
    print()
    print(
        "Tip: on Bazzite/Steam Deck clients, these media UDP ports are required for a video window."
    )
    print()

    if which("firewall-cmd"):
        state = subprocess.run(
            ["firewall-cmd", "--state"],
            capture_output=True,
            text=True,
            check=False,
        )
        if state.returncode == 0:
            run(["firewall-cmd", "--permanent", f"--add-port={control_tcp}/tcp"])
            run(["firewall-cmd", "--permanent", f"--add-port={input_udp}/udp"])
            run(["firewall-cmd", "--permanent", f"--add-port={discovery_udp}/udp"])
            run(
                [
                    "firewall-cmd",
                    "--permanent",
                    f"--add-port={video_start}-{video_end}/udp",
                ]
            )
            run(
                [
                    "firewall-cmd",
                    "--permanent",
                    f"--add-port={audio_start}-{audio_end}/udp",
                ]
            )
            run(["firewall-cmd", "--reload"])
            print("Done (firewalld). Bazzite/Fedora: ports opened permanently.")
            return 0

    if which("ufw"):
        run(["ufw", "allow", f"{control_tcp}/tcp", "comment", "ArchStreamer control"])
        run(["ufw", "allow", f"{input_udp}/udp", "comment", "ArchStreamer input"])
        run(
            [
                "ufw",
                "allow",
                f"{discovery_udp}/udp",
                "comment",
                "ArchStreamer discovery",
            ]
        )
        run(
            [
                "ufw",
                "allow",
                f"{video_start}:{video_end}/udp",
                "comment",
                "ArchStreamer video RTP",
            ]
        )
        run(
            [
                "ufw",
                "allow",
                f"{audio_start}:{audio_end}/udp",
                "comment",
                "ArchStreamer audio RTP",
            ]
        )
        status = subprocess.run(
            ["ufw", "status", "numbered"],
            capture_output=True,
            text=True,
            check=False,
        )
        for line in (status.stdout or "").splitlines()[:40]:
            print(line)
        print()
        print("If UFW was inactive, enable with: sudo ufw enable")
        print("Done (ufw).")
        return 0

    if which("nft"):
        tables = subprocess.run(
            ["nft", "list", "tables"],
            capture_output=True,
            text=True,
            check=False,
        )
        if "inet archstreamer" in (tables.stdout or ""):
            run(["nft", "delete", "table", "inet", "archstreamer"], check=False)
        run(["nft", "add", "table", "inet", "archstreamer"])
        run(
            [
                "nft",
                "add",
                "chain",
                "inet",
                "archstreamer",
                "input",
                "{ type filter hook input priority 0; policy accept; }",
            ]
        )
        run(
            [
                "nft",
                "add",
                "rule",
                "inet",
                "archstreamer",
                "input",
                "tcp",
                "dport",
                str(control_tcp),
                "accept",
                "comment",
                "ArchStreamer control",
            ]
        )
        run(
            [
                "nft",
                "add",
                "rule",
                "inet",
                "archstreamer",
                "input",
                "udp",
                "dport",
                str(input_udp),
                "accept",
                "comment",
                "ArchStreamer input",
            ]
        )
        run(
            [
                "nft",
                "add",
                "rule",
                "inet",
                "archstreamer",
                "input",
                "udp",
                "dport",
                str(discovery_udp),
                "accept",
                "comment",
                "ArchStreamer discovery",
            ]
        )
        run(
            [
                "nft",
                "add",
                "rule",
                "inet",
                "archstreamer",
                "input",
                "udp",
                "dport",
                f"{video_start}-{video_end}",
                "accept",
                "comment",
                "ArchStreamer video",
            ]
        )
        run(
            [
                "nft",
                "add",
                "rule",
                "inet",
                "archstreamer",
                "input",
                "udp",
                "dport",
                f"{audio_start}-{audio_end}",
                "accept",
                "comment",
                "ArchStreamer audio",
            ]
        )
        run(["nft", "list", "table", "inet", "archstreamer"])
        print("Done (nftables table inet archstreamer).")
        return 0

    eprint("Neither firewalld, ufw, nor nft found.")
    eprint(
        f"Manually open: TCP {control_tcp}, UDP {input_udp}, UDP {discovery_udp},"
    )
    eprint(
        f"  UDP {video_start}-{video_end}, UDP {audio_start}-{audio_end}"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
