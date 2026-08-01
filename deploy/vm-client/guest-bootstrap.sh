#!/usr/bin/env bash
# Bootstrap ArchStreamer client build inside the Ubuntu VM.
# Run after the desktop install finishes, from a terminal in the guest.
#
# Usage:
#   bash deploy/vm-client/guest-bootstrap.sh
#   bash deploy/vm-client/guest-bootstrap.sh --branch dev
#   ARCHSTREAMER_REPO_BRANCH=dev bash deploy/vm-client/guest-bootstrap.sh
#
# Defaults to the deploy branch (master). Use --branch to test another ref.
set -euo pipefail

REPO_URL="${ARCHSTREAMER_REPO_URL:-https://github.com/ArisenPhoenix/RetroStreamer.git}"
REPO_DIR="${ARCHSTREAMER_REPO_DIR:-$HOME/ArchStreamer}"
HOST_IP="${ARCHSTREAMER_HOST_IP:-192.168.122.1}"
BRANCH="${ARCHSTREAMER_REPO_BRANCH:-master}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Clone/update ArchStreamer and build a client-only tree.

Options:
  -b, --branch NAME   Git branch to use (default: master, or \$ARCHSTREAMER_REPO_BRANCH)
  -h, --help          Show this help

Environment:
  ARCHSTREAMER_REPO_URL      Clone URL (default: GitHub RetroStreamer)
  ARCHSTREAMER_REPO_DIR      Checkout path (default: \$HOME/ArchStreamer)
  ARCHSTREAMER_REPO_BRANCH   Same as --branch
  ARCHSTREAMER_HOST_IP       Printed in the join example (default: 192.168.122.1)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--branch)
      [[ $# -ge 2 ]] || { echo "error: $1 requires a branch name" >&2; exit 2; }
      BRANCH="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${BRANCH}" ]]; then
  echo "error: branch name must not be empty" >&2
  exit 2
fi

echo "==> Installing build and runtime dependencies"
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential \
  cmake \
  pkg-config \
  git \
  ninja-build \
  libsdl2-dev \
  qt6-base-dev \
  libgstreamer1.0-0 \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  gstreamer1.0-x \
  gstreamer1.0-pulseaudio \
  pulseaudio-utils \
  ca-certificates

echo "==> Cloning / updating repo at ${REPO_DIR} (branch: ${BRANCH})"
if [[ -d "${REPO_DIR}/.git" ]]; then
  git -C "${REPO_DIR}" fetch --all --prune
  git -C "${REPO_DIR}" checkout "${BRANCH}"
  # Prefer matching origin/<branch>; fall back to ff-only pull if remote tracking is odd.
  if git -C "${REPO_DIR}" rev-parse --verify "origin/${BRANCH}" >/dev/null 2>&1; then
    git -C "${REPO_DIR}" reset --hard "origin/${BRANCH}"
  else
    git -C "${REPO_DIR}" pull --ff-only origin "${BRANCH}"
  fi
else
  git clone --branch "${BRANCH}" --single-branch "${REPO_URL}" "${REPO_DIR}"
fi
echo "==> Git: $(git -C "${REPO_DIR}" rev-parse --short HEAD) $(git -C "${REPO_DIR}" log -1 --pretty=%s)"

echo "==> Configuring client-capable build (ARCHSTREAMER_BUILD_HOST=OFF)"
cmake -S "${REPO_DIR}" -B "${REPO_DIR}/build" \
  -G Ninja \
  -DARCHSTREAMER_BUILD_HOST=OFF \
  -DCMAKE_BUILD_TYPE=Release

echo "==> Building"
cmake --build "${REPO_DIR}/build" -j"$(nproc)"

cat <<EOF

Bootstrap complete (branch: ${BRANCH}).

Metal host (on the other side of libvirt NAT) is usually:
  ${HOST_IP}

Example CLI join (from this guest):
  ${REPO_DIR}/build/session_client \\
    --host ${HOST_IP} --port 45555 --input-port 45454 \\
    --username vm_client --role player --mode singleplayer --players 1 \\
    --game 0 --synced-av

Or run the GUI:
  ${REPO_DIR}/build/archstreamer_gui

In the GUI Client tab set Host=${HOST_IP}, enable Synced A/V if comparing lip-sync,
and Connect / Join Session while the metal Host tab is running.

EOF
