#!/usr/bin/env bash
# Copy client binaries from the metal build tree into the ArchStreamer client VM.
# Avoids rebuilding inside the guest when only metal sources changed.
#
# Run on the metal host:
#   ./deploy/vm-client/copy-gui-to-vm.sh
#   ./deploy/vm-client/copy-gui-to-vm.sh --build
#   ./deploy/vm-client/copy-gui-to-vm.sh --also-session-client
#
# Auth (first match wins):
#   1) SSH key already authorized on the guest
#   2) ARCHSTREAMER_VM_PASSWORD / SSHPASS + sshpass (non-interactive)
#   3) Interactive password prompt (TTY required)
#
# Env overrides:
#   ARCHSTREAMER_VM_NAME     libvirt domain (default: archstreamer-client)
#   ARCHSTREAMER_VM_USER     SSH user (default: merk_virt)
#   ARCHSTREAMER_VM_HOST     guest IP (default: dhcp lease lookup, else 192.168.122.6)
#   ARCHSTREAMER_VM_DIR      remote build dir (default: Documents/RetroStreamer/build)
#   ARCHSTREAMER_BUILD_DIR   local build dir (default: <repo>/build)
#   ARCHSTREAMER_VM_PASSWORD guest password (optional; prefer keys long-term)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

DO_BUILD=0
ALSO_SESSION_CLIENT=0
for arg in "$@"; do
  case "$arg" in
    --build) DO_BUILD=1 ;;
    --also-session-client) ALSO_SESSION_CLIENT=1 ;;
    -h|--help)
      sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "Unknown argument: $arg (try --help)" >&2
      exit 1
      ;;
  esac
done

VM_NAME="${ARCHSTREAMER_VM_NAME:-archstreamer-client}"
VM_USER="${ARCHSTREAMER_VM_USER:-merk_virt}"
VM_DIR="${ARCHSTREAMER_VM_DIR:-Documents/RetroStreamer/build}"
BUILD_DIR="${ARCHSTREAMER_BUILD_DIR:-${REPO_ROOT}/build}"
VM_PASSWORD="${ARCHSTREAMER_VM_PASSWORD:-${SSHPASS:-}}"

SSH_BASE_OPTS=(
  -o StrictHostKeyChecking=accept-new
  -o PreferredAuthentications=publickey,password,keyboard-interactive
  -o PubkeyAuthentication=yes
  -o PasswordAuthentication=yes
)

run_virsh() {
  if id -nG 2>/dev/null | grep -qw libvirt; then
    virsh -c qemu:///system "$@"
  else
    sg libvirt -c "virsh -c qemu:///system $*"
  fi
}

resolve_vm_host() {
  if [[ -n "${ARCHSTREAMER_VM_HOST:-}" ]]; then
    echo "${ARCHSTREAMER_VM_HOST}"
    return
  fi

  local lease_ip=""
  lease_ip="$(run_virsh net-dhcp-leases default 2>/dev/null | awk '
    BEGIN { IGNORECASE = 1 }
    NR <= 2 { next }
    $0 ~ /(archstreamer-client|merk-virt|merk_virt)/ {
      split($5, a, "/"); print a[1]; exit
    }
  ' || true)"
  if [[ -n "$lease_ip" ]]; then
    echo "$lease_ip"
    return
  fi

  lease_ip="$(run_virsh net-dhcp-leases default 2>/dev/null | awk '
    NR <= 2 { next }
    $4 == "ipv4" {
      split($5, a, "/"); print a[1]; exit
    }
  ' || true)"
  if [[ -n "$lease_ip" ]]; then
    echo "$lease_ip"
    return
  fi

  echo "192.168.122.6"
}

ensure_sshpass() {
  if command -v sshpass >/dev/null 2>&1; then
    return 0
  fi
  echo "==> Installing sshpass for password-based copy"
  sudo apt-get install -y sshpass
}

# remote_ssh|remote_scp wrappers pick key / sshpass / interactive.
AUTH_MODE="" # key | password | interactive

detect_auth() {
  local remote="$1"
  if ssh -o BatchMode=yes -o ConnectTimeout=3 "${SSH_BASE_OPTS[@]}" \
      "${remote}" "true" 2>/dev/null; then
    AUTH_MODE=key
    return
  fi
  if [[ -n "$VM_PASSWORD" ]]; then
    ensure_sshpass
    if SSHPASS="$VM_PASSWORD" sshpass -e \
        ssh -o BatchMode=yes -o ConnectTimeout=5 "${SSH_BASE_OPTS[@]}" \
        -o NumberOfPasswordPrompts=1 \
        "${remote}" "true" 2>/dev/null; then
      AUTH_MODE=password
      return
    fi
    echo "Password auth failed for ${remote}." >&2
    echo "Check ARCHSTREAMER_VM_PASSWORD / user / host." >&2
    exit 1
  fi
  if [[ -t 0 ]]; then
    AUTH_MODE=interactive
    return
  fi
  echo "No SSH key and no ARCHSTREAMER_VM_PASSWORD, and stdin is not a TTY." >&2
  echo "Either:" >&2
  echo "  ssh-copy-id ${remote}" >&2
  echo "  ARCHSTREAMER_VM_PASSWORD='…' $0" >&2
  exit 1
}

remote_ssh() {
  local remote="$1"
  shift
  case "$AUTH_MODE" in
    key)
      ssh "${SSH_BASE_OPTS[@]}" "${remote}" "$@"
      ;;
    password)
      SSHPASS="$VM_PASSWORD" sshpass -e \
        ssh "${SSH_BASE_OPTS[@]}" -o NumberOfPasswordPrompts=1 "${remote}" "$@"
      ;;
    interactive)
      ssh "${SSH_BASE_OPTS[@]}" "${remote}" "$@"
      ;;
  esac
}

remote_scp() {
  local src="$1"
  local dst="$2"
  case "$AUTH_MODE" in
    key)
      scp "${SSH_BASE_OPTS[@]}" -q "$src" "$dst"
      ;;
    password)
      SSHPASS="$VM_PASSWORD" sshpass -e \
        scp "${SSH_BASE_OPTS[@]}" -o NumberOfPasswordPrompts=1 -q "$src" "$dst"
      ;;
    interactive)
      scp "${SSH_BASE_OPTS[@]}" -q "$src" "$dst"
      ;;
  esac
}

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "==> Building archstreamer_gui on metal"
  cmake --build "${BUILD_DIR}" -j"$(nproc)" --target archstreamer_gui
  if [[ "$ALSO_SESSION_CLIENT" -eq 1 ]]; then
    cmake --build "${BUILD_DIR}" -j"$(nproc)" --target session_client
  fi
fi

GUI_BIN="${BUILD_DIR}/archstreamer_gui"
if [[ ! -x "$GUI_BIN" ]]; then
  echo "Missing executable: ${GUI_BIN}" >&2
  echo "Build first, or pass --build." >&2
  exit 1
fi

FILES=("$GUI_BIN")
if [[ "$ALSO_SESSION_CLIENT" -eq 1 ]]; then
  SC_BIN="${BUILD_DIR}/session_client"
  if [[ ! -x "$SC_BIN" ]]; then
    echo "Missing executable: ${SC_BIN}" >&2
    echo "Build session_client first, or pass --build --also-session-client." >&2
    exit 1
  fi
  FILES+=("$SC_BIN")
fi

VM_HOST="$(resolve_vm_host)"
REMOTE="${VM_USER}@${VM_HOST}"
REMOTE_DIR="\$HOME/${VM_DIR#\~/}"

echo "==> Target ${REMOTE}:${REMOTE_DIR}/"
echo "==> Binaries: ${FILES[*]}"

run_virsh start "${VM_NAME}" >/dev/null 2>&1 || true

echo "==> Waiting for guest SSH…"
ready=0
for _ in $(seq 1 30); do
  if nc -z -w 1 "${VM_HOST}" 22 2>/dev/null || \
     timeout 1 bash -c "echo >/dev/tcp/${VM_HOST}/22" 2>/dev/null; then
    ready=1
    break
  fi
  sleep 1
done
if [[ "$ready" -ne 1 ]]; then
  echo "SSH port not open on ${VM_HOST}:22" >&2
  exit 1
fi

detect_auth "${REMOTE}"
echo "==> Auth mode: ${AUTH_MODE}"

remote_ssh "${REMOTE}" "mkdir -p ${REMOTE_DIR}"

for local_path in "${FILES[@]}"; do
  base="$(basename "$local_path")"
  echo "==> Copying ${base}"
  remote_scp "$local_path" "${REMOTE}:/tmp/${base}.archstreamer.new"
  remote_ssh "${REMOTE}" \
    "mv -f /tmp/${base}.archstreamer.new ${REMOTE_DIR}/${base} && chmod +x ${REMOTE_DIR}/${base}"
done

# Best-effort: install metal public key so later copies need no password.
if [[ "$AUTH_MODE" == "password" ]] && [[ -f "${HOME}/.ssh/id_ed25519.pub" || -f "${HOME}/.ssh/id_rsa.pub" ]]; then
  echo "==> Installing metal SSH public key on guest (so next run can use key auth)"
  PUB=""
  [[ -f "${HOME}/.ssh/id_ed25519.pub" ]] && PUB="${HOME}/.ssh/id_ed25519.pub"
  [[ -z "$PUB" && -f "${HOME}/.ssh/id_rsa.pub" ]] && PUB="${HOME}/.ssh/id_rsa.pub"
  if [[ -n "$PUB" ]]; then
    KEY_DATA="$(cat "$PUB")"
    remote_ssh "${REMOTE}" \
      "mkdir -p ~/.ssh && chmod 700 ~/.ssh && touch ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && grep -qxF '${KEY_DATA}' ~/.ssh/authorized_keys || echo '${KEY_DATA}' >> ~/.ssh/authorized_keys"
  fi
fi

echo
echo "Done. On the guest, restart the GUI if it was already running:"
echo "  ~/${VM_DIR#\~/}/archstreamer_gui"
echo
echo "Client Host field should be 192.168.122.1 (metal NAT gateway)."
