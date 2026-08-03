#!/usr/bin/env python3
"""Emulate two computers: metal host_runner + Docker client on a bridge network."""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

from scriptutil import eprint, repo_root, run


def main(argv: list[str] | None = None) -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/lan-sim-smoke.sh",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=root / "build",
    )
    parser.add_argument(
        "--control-port",
        type=int,
        default=int(os.environ.get("CONTROL_PORT", "45555")),
    )
    parser.add_argument(
        "--input-port",
        type=int,
        default=int(os.environ.get("INPUT_PORT", "45454")),
    )
    parser.add_argument(
        "--host-gateway",
        default="172.30.0.1",
    )
    parser.add_argument(
        "--client-ip",
        default="172.30.0.20",
    )
    args = parser.parse_args(argv)

    build: Path = args.build_dir
    compose_dir = root / "deploy/lan-sim"
    host_bin = build / "host_runner"
    client_bin = build / "session_client"
    control_port = args.control_port
    input_port = args.input_port
    host_gateway = args.host_gateway
    client_ip = args.client_ip

    if not (
        host_bin.is_file()
        and os.access(host_bin, os.X_OK)
        and client_bin.is_file()
        and os.access(client_bin, os.X_OK)
    ):
        eprint(f"Build first: cmake --build {build} -j$(nproc)")
        return 1

    host_log = compose_dir / "host.log"
    client_log = compose_dir / "client.log"
    host_proc: subprocess.Popen[str] | None = None
    client_proc: subprocess.Popen[str] | None = None

    def cleanup() -> None:
        nonlocal host_proc, client_proc
        if client_proc is not None and client_proc.poll() is None:
            try:
                client_proc.kill()
            except OSError:
                pass
            try:
                client_proc.wait(timeout=5)
            except Exception:
                pass
        if host_proc is not None and host_proc.poll() is None:
            try:
                host_proc.send_signal(signal.SIGTERM)
            except OSError:
                pass
            try:
                host_proc.wait(timeout=5)
            except Exception:
                try:
                    host_proc.kill()
                except OSError:
                    pass
        subprocess.run(
            ["docker", "compose", "down", "--remove-orphans"],
            cwd=str(compose_dir),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )

    try:
        print("==> Building/starting Docker client (second computer on 172.30.0.0/24)")
        run(["docker", "compose", "up", "-d", "--build"], cwd=compose_dir)

        print("==> Waiting for container network")
        for _ in range(30):
            probe = subprocess.run(
                ["docker", "exec", "archstreamer-client", "true"],
                capture_output=True,
                check=False,
            )
            if probe.returncode == 0:
                break
            time.sleep(0.5)

        print(
            "==> Starting host_runner on metal (clients=1, viewer host, dry catalog wait)"
        )
        host_log.write_text("")
        host_proc = subprocess.Popen(
            [
                str(host_bin),
                "--control-port",
                str(control_port),
                "--input-port",
                str(input_port),
                "--clients",
                "1",
                "--host-role",
                "viewer",
                "--mode",
                "singleplayer",
                "--session-timeout",
                "45",
            ],
            stdout=host_log.open("w"),
            stderr=subprocess.STDOUT,
            text=True,
        )

        time.sleep(1)
        if host_proc.poll() is not None:
            eprint("host_runner failed to start:")
            eprint(host_log.read_text())
            return 1

        print(
            f"==> Client connecting from {client_ip} -> host {host_gateway}:{control_port}"
        )
        client_log.write_text("")
        client_proc = subprocess.Popen(
            [
                "docker",
                "exec",
                "archstreamer-client",
                "./session_client",
                "--host",
                host_gateway,
                "--port",
                str(control_port),
                "--input-port",
                str(input_port),
                "--username",
                "kid_vm",
                "--role",
                "viewer",
                "--mode",
                "singleplayer",
                "--players",
                "0",
                "--game",
                "0",
            ],
            stdout=client_log.open("w"),
            stderr=subprocess.STDOUT,
            text=True,
        )

        deadline = time.monotonic() + 40
        ok = False
        catalog_re = re.compile(r"Received .* games from host")
        while time.monotonic() < deadline:
            try:
                text = client_log.read_text()
            except OSError:
                text = ""
            if catalog_re.search(text):
                ok = True
                break
            if host_proc.poll() is not None:
                break
            time.sleep(0.5)

        print()
        print("---- host.log (tail) ----")
        host_lines = host_log.read_text().splitlines()
        for line in host_lines[-40:]:
            print(line)
        print("---- client.log (tail) ----")
        client_lines = client_log.read_text().splitlines()
        for line in client_lines[-40:]:
            print(line)
        print()

        if client_proc.poll() is None:
            try:
                client_proc.kill()
            except OSError:
                pass
            try:
                client_proc.wait(timeout=5)
            except Exception:
                pass
            client_proc = None

        if ok:
            print(
                "LAN-sim SUCCESS: container client fetched catalog from metal host over 172.30.0.0/24."
            )
            print(
                "Note: Docker has no gamepad; use a real second PC (or USB passthrough) for player input."
            )
            return 0

        eprint("LAN-sim FAILED: no catalog/join evidence. Check firewall with:")
        eprint(f"  sudo {root}/scripts/apply-firewall.sh")
        return 1
    finally:
        cleanup()


if __name__ == "__main__":
    sys.exit(main())
