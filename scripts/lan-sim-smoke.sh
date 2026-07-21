#!/usr/bin/env bash
# Emulate two computers: metal host_runner + Docker client on a bridge network.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
COMPOSE_DIR="${ROOT}/deploy/lan-sim"
HOST_BIN="${BUILD}/host_runner"
CLIENT_BIN="${BUILD}/session_client"

CONTROL_PORT="${CONTROL_PORT:-45555}"
INPUT_PORT="${INPUT_PORT:-45454}"
HOST_GATEWAY="172.30.0.1"
CLIENT_IP="172.30.0.20"

if [[ ! -x "${HOST_BIN}" || ! -x "${CLIENT_BIN}" ]]; then
  echo "Build first: cmake --build ${BUILD} -j\$(nproc)" >&2
  exit 1
fi

cleanup() {
  if [[ -n "${HOST_PID:-}" ]]; then
    kill "${HOST_PID}" 2>/dev/null || true
    wait "${HOST_PID}" 2>/dev/null || true
  fi
  (cd "${COMPOSE_DIR}" && docker compose down --remove-orphans >/dev/null 2>&1) || true
}
trap cleanup EXIT

echo "==> Building/starting Docker client (second computer on 172.30.0.0/24)"
(cd "${COMPOSE_DIR}" && docker compose up -d --build)

echo "==> Waiting for container network"
for _ in $(seq 1 30); do
  if docker exec archstreamer-client true 2>/dev/null; then
    break
  fi
  sleep 0.5
done

echo "==> Starting host_runner on metal (clients=1, viewer host, dry catalog wait)"
# Session mode waits for a player; we join from the container.
"${HOST_BIN}" \
  --control-port "${CONTROL_PORT}" \
  --input-port "${INPUT_PORT}" \
  --clients 1 \
  --host-role viewer \
  --mode singleplayer \
  --session-timeout 45 \
  >"${ROOT}/deploy/lan-sim/host.log" 2>&1 &
HOST_PID=$!

sleep 1
if ! kill -0 "${HOST_PID}" 2>/dev/null; then
  echo "host_runner failed to start:" >&2
  cat "${ROOT}/deploy/lan-sim/host.log" >&2
  exit 1
fi

echo "==> Client connecting from ${CLIENT_IP} -> host ${HOST_GATEWAY}:${CONTROL_PORT}"
set +e
docker exec archstreamer-client ./session_client \
  --host "${HOST_GATEWAY}" \
  --port "${CONTROL_PORT}" \
  --input-port "${INPUT_PORT}" \
  --username kid_vm \
  --role viewer \
  --mode singleplayer \
  --players 0 \
  --game 0 \
  >"${ROOT}/deploy/lan-sim/client.log" 2>&1 &
CLIENT_PID=$!

# Network proof = catalog over the Docker bridge. Full lobby start needs a player seat.
deadline=$((SECONDS + 40))
ok=0
while (( SECONDS < deadline )); do
  if grep -Eq "Received .* games from host" "${ROOT}/deploy/lan-sim/client.log" 2>/dev/null; then
    ok=1
    break
  fi
  if ! kill -0 "${HOST_PID}" 2>/dev/null; then
    break
  fi
  sleep 0.5
done
set -e

echo
echo "---- host.log (tail) ----"
tail -n 40 "${ROOT}/deploy/lan-sim/host.log" || true
echo "---- client.log (tail) ----"
tail -n 40 "${ROOT}/deploy/lan-sim/client.log" || true
echo

kill "${CLIENT_PID}" 2>/dev/null || true
wait "${CLIENT_PID}" 2>/dev/null || true

if [[ "${ok}" -eq 1 ]]; then
  echo "LAN-sim SUCCESS: container client fetched catalog from metal host over 172.30.0.0/24."
  echo "Note: Docker has no gamepad; use a real second PC (or USB passthrough) for player input."
  exit 0
fi

echo "LAN-sim FAILED: no catalog/join evidence. Check firewall with:" >&2
echo "  sudo ${ROOT}/scripts/apply-firewall.sh" >&2
exit 1
