#!/usr/bin/env python3
"""Sample ArchStreamer host traffic for N minutes (TCP peers, RTP fanout, encode pps).

Usage:
  scripts/monitor_archstreamer_traffic.py              # 10 minutes
  scripts/monitor_archstreamer_traffic.py --minutes 5
  scripts/monitor_archstreamer_traffic.py -m 15 --out /tmp/as-mon

Looks for host_runner on 45555/45565/45575, gst-launch multiudpsink fanouts,
and localhost video RTP on :5004 when present. Writes a TSV + event log.
"""

from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

from scriptutil import eprint, repo_root

CONTROL_PORTS = (45555, 45565, 45575)
HOST_LOG_KEYS = (
    "client_joined",
    "client_left",
    "disconnected",
    "ClientSessionLeave",
    "ERROR",
    "Failed",
    "Video ladder",
    "Video cutover",
    "Staging video",
    "Accepted control",
    "session_ended",
    "session slot",
    "Connection reset",
    "Saved client",
    "Lobby presence",
)


def sh(cmd: str) -> str:
    try:
        return subprocess.check_output(
            cmd,
            shell=True,
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        return ""


def tcp_peers() -> list[str]:
    filt = " or ".join(f"sport = :{p}" for p in CONTROL_PORTS)
    text = sh(f"ss -tn state established '( {filt} )'")
    peers: list[str] = []
    for line in text.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 4:
            peers.append(parts[3])
    return sorted(set(peers))


def gst_fanouts() -> tuple[list[str], list[str]]:
    text = sh("ps -ww -C gst-launch-1.0 -o args=")
    videos: list[str] = []
    audios: list[str] = []
    for line in text.splitlines():
        m = re.search(r"multiudpsink clients=(\S+)", line)
        if not m:
            continue
        clients = m.group(1)
        if "rtph264pay" in line or "nvh264enc" in line or "pipewiresrc" in line:
            videos.append(clients)
        elif "rtpopuspay" in line or "opusenc" in line or "pulsesrc" in line:
            audios.append(clients)
    return videos, audios


def lo_video_pps(seconds: float = 0.5) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("127.0.0.1", 5004))
    except OSError:
        sock.close()
        return -1
    sock.settimeout(0.15)
    n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            sock.recvfrom(65535)
            n += 1
        except socket.timeout:
            pass
    sock.close()
    return int(n / max(seconds, 0.01))


def classify(peers: list[str]) -> str:
    joined = ",".join(peers)
    has_wg = "10.6.0." in joined
    has_lan = "192.168.100." in joined
    if has_wg and has_lan:
        return "mixed"
    if has_wg:
        return "wireguard"
    if has_lan:
        return "lan"
    if peers:
        return "other"
    return "none"


def newest_host_log(build: Path) -> Path | None:
    logs = sorted(build.glob("host_*.log"), key=lambda p: p.stat().st_mtime, reverse=True)
    return logs[0] if logs else None


def main(argv: list[str] | None = None) -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-m",
        "--minutes",
        type=float,
        default=10.0,
        help="How long to sample (default: 10)",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="Seconds between samples (default: 1)",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=root / "build",
        help="Directory with host_*.log / host_runner",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("/tmp/archstreamer-monitor"),
        help="Output directory for TSV + events",
    )
    args = parser.parse_args(argv)

    minutes = max(0.1, float(args.minutes))
    interval = max(0.2, float(args.interval))
    out_dir: Path = args.out
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    tsv_path = out_dir / f"session_{stamp}.tsv"
    events_path = out_dir / f"events_{stamp}.log"

    def note(msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        with events_path.open("a", encoding="utf-8") as fh:
            fh.write(line + "\n")
        print(line, flush=True)

    with tsv_path.open("w", encoding="utf-8") as fh:
        fh.write(
            "ts_epoch\tts_local\ttcp_peers\tvideo_fanouts\taudio_fanouts\t"
            "lo_v_pps\tgst_v_n\tgst_a_n\tpath\n"
        )

    end = time.time() + minutes * 60.0
    host_log = newest_host_log(args.build_dir)
    prev_log_n = (
        len(host_log.read_text(encoding="utf-8", errors="replace").splitlines())
        if host_log and host_log.exists()
        else 0
    )
    prev_peers: str | None = None
    prev_v: str | None = None
    prev_a: str | None = None
    last_zero_note = 0.0
    last_missing_note = 0.0
    samples = 0

    note(
        f"monitor start minutes={minutes} interval={interval}s "
        f"build={args.build_dir} tsv={tsv_path}"
    )
    if host_log:
        note(f"host_log={host_log.name} offset={prev_log_n}")

    try:
        while time.time() < end:
            logs = newest_host_log(args.build_dir)
            if logs and logs != host_log:
                host_log = logs
                prev_log_n = len(
                    host_log.read_text(encoding="utf-8", errors="replace").splitlines()
                )
                note(f"switched host_log -> {host_log.name}")

            peers = tcp_peers()
            videos, audios = gst_fanouts()
            vpps = lo_video_pps(0.45) if videos else 0
            path = classify(peers)
            peer_s = ",".join(peers) if peers else "-"
            v_s = ";".join(videos) if videos else "-"
            a_s = ";".join(audios) if audios else "-"

            if host_log and host_log.exists():
                lines = host_log.read_text(encoding="utf-8", errors="replace").splitlines()
                if len(lines) > prev_log_n:
                    for line in lines[prev_log_n:]:
                        if any(k in line for k in HOST_LOG_KEYS):
                            note(f"HOST[{host_log.name}] {line[:200]}")
                    prev_log_n = len(lines)

            if peer_s != prev_peers:
                note(f"TCP peers: {prev_peers} -> {peer_s} ({path})")
                prev_peers = peer_s
            if v_s != prev_v:
                note(f"video fanout: {v_s}")
                prev_v = v_s
            if a_s != prev_a:
                note(f"audio fanout: {a_s}")
                prev_a = a_s

            now = time.time()
            if peers and not videos and now - last_missing_note > 15:
                note("video gst missing while TCP peer connected")
                last_missing_note = now
            if videos and vpps == 0 and now - last_zero_note > 15:
                note("lo video pps=0 while gst running (remote-only ladder or stall)")
                last_zero_note = now
            elif videos and 0 < vpps < 40 and now - last_zero_note > 15:
                note(f"lo video low pps={vpps}")
                last_zero_note = now

            with tsv_path.open("a", encoding="utf-8") as fh:
                fh.write(
                    f"{int(now)}\t{time.strftime('%H:%M:%S')}\t{peer_s}\t{v_s}\t{a_s}\t"
                    f"{vpps}\t{len(videos)}\t{len(audios)}\t{path}\n"
                )
            samples += 1
            time.sleep(interval)
    except KeyboardInterrupt:
        note("interrupted")

    # Summary
    rows: list[list[str]] = []
    for line in tsv_path.read_text(encoding="utf-8").splitlines()[1:]:
        parts = line.split("\t")
        if len(parts) >= 9:
            rows.append(parts)
    paths = Counter(r[8] for r in rows)
    note(
        f"monitor done samples={samples} paths={dict(paths)} "
        f"events={events_path} tsv={tsv_path}"
    )
    eprint(f"Wrote {tsv_path}")
    eprint(f"Wrote {events_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
