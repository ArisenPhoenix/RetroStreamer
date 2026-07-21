# LAN simulation (second computer via Docker)

This repo does not require a second physical PC for basic networking checks.
Docker provides an isolated network namespace that acts as the remote client.

## Layout

| Role | Where | Address |
|------|--------|---------|
| Host | metal (`host_runner` / GUI) | advertises / listens on all interfaces; Docker gateway `172.30.0.1` |
| Client | container `archstreamer-client` | `172.30.0.20` on bridge `archstreamer_lan` |

Artwork stays local on each machine (not transferred). Discovery UDP broadcast across Docker bridges is limited; use the gateway IP for smoke tests, then validate username discovery on real LAN / Wi‑Fi.

## Firewall (metal host)

```bash
sudo ./scripts/apply-firewall.sh
```

Opens:

- `45555/tcp` control
- `45454/udp` input
- `45550/udp` discovery
- `5004–5011/udp` video RTP (span configurable)
- `6004–6011/udp` audio RTP

## One-shot smoke

```bash
cmake --build build -j"$(nproc)"
./scripts/lan-sim-smoke.sh
```

This starts the Docker client, runs `host_runner` on the host, and joins from the container to `172.30.0.1`.

## Manual interactive client

```bash
cd deploy/lan-sim
docker compose up -d --build
docker exec -it archstreamer-client ./session_client \
  --host 172.30.0.1 --port 45555 --input-port 45454 \
  --username kid_vm --role player --mode singleplayer --players 1 --game 0
```

In another terminal on the metal machine:

```bash
./build/host_runner --control-port 45555 --input-port 45454 --clients 1 --host-role viewer
```

## Why Docker instead of a full VM

QEMU/libvirt are not installed on this machine. Docker already works, gives a separate IP stack, and is enough to exercise TCP control, UDP input, and RTP return paths before bringing up a second PC. A full VM can be added later if you need a complete desktop client OS.
