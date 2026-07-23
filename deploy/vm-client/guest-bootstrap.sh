#!/usr/bin/env bash
# Bootstrap ArchStreamer client build inside the Ubuntu VM.
# Run after the desktop install finishes, from a terminal in the guest.
set -euo pipefail

REPO_URL="${ARCHSTREAMER_REPO_URL:-https://github.com/ArisenPhoenix/RetroStreamer.git}"
REPO_DIR="${ARCHSTREAMER_REPO_DIR:-$HOME/ArchStreamer}"
HOST_IP="${ARCHSTREAMER_HOST_IP:-192.168.122.1}"

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

echo "==> Cloning / updating repo at ${REPO_DIR}"
if [[ -d "${REPO_DIR}/.git" ]]; then
  git -C "${REPO_DIR}" fetch --all --prune
  git -C "${REPO_DIR}" pull --ff-only || true
else
  git clone "${REPO_URL}" "${REPO_DIR}"
fi

echo "==> Configuring client-capable build (ARCHSTREAMER_BUILD_HOST=OFF)"
cmake -S "${REPO_DIR}" -B "${REPO_DIR}/build" \
  -G Ninja \
  -DARCHSTREAMER_BUILD_HOST=OFF \
  -DCMAKE_BUILD_TYPE=Release

echo "==> Building"
cmake --build "${REPO_DIR}/build" -j"$(nproc)"

cat <<EOF

Bootstrap complete.

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
