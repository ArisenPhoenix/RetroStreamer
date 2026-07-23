#!/usr/bin/env bash
# Open (or start) the ArchStreamer client VM console on the metal host.
set -euo pipefail

NAME="${1:-archstreamer-client}"

if ! id -nG | grep -qw libvirt; then
  exec sg libvirt -c "bash '$0' $*"
fi

virsh -c qemu:///system start "${NAME}" 2>/dev/null || true
exec virt-viewer --connect qemu:///system --attach "${NAME}"
