#!/usr/bin/env bash
# One-time / repair setup on the metal host for the ArchStreamer client VM.
# Prefer: pkexec $0   or   sudo $0
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Re-running with pkexec..."
  exec pkexec env "HOME=${HOME}" "USER=${USER}" bash "$0" "$@"
fi

REAL_USER="${SUDO_USER:-${USER}}"
REAL_HOME="$(getent passwd "${REAL_USER}" | cut -d: -f6)"
VM_DIR="${ARCHSTREAMER_VM_DIR:-/mnt/Internal_SSD/Programming/Mixed/ArchStreamer-VMs/archstreamer-client}"
ISO_NAME="ubuntu-24.04.2-desktop-amd64.iso"
ISO_URL="https://releases.ubuntu.com/24.04.2/${ISO_NAME}"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y \
  qemu-system-x86 qemu-kvm \
  libvirt-daemon-system libvirt-clients \
  virt-manager virt-viewer virtinst bridge-utils

usermod -aG libvirt,kvm "${REAL_USER}" || true

# Default NAT network (guest -> 192.168.122.1 host)
if [[ -f /etc/libvirt/qemu/networks/default.xml ]]; then
  virsh net-define /etc/libvirt/qemu/networks/default.xml 2>/dev/null || true
  virsh net-autostart default
  virsh net-start default 2>/dev/null || true
fi
ip link set virbr0 up 2>/dev/null || true

mkdir -p "${VM_DIR}"
# libvirt-qemu must traverse /mnt/... to the VM files on the SSD
for d in /mnt /mnt/Internal_SSD /mnt/Internal_SSD/Programming \
         /mnt/Internal_SSD/Programming/Mixed \
         /mnt/Internal_SSD/Programming/Mixed/ArchStreamer-VMs; do
  if [[ -d "${d}" ]]; then
    setfacl -m u:libvirt-qemu:--x "${d}" || true
  fi
done
setfacl -R -m u:libvirt-qemu:rwx "${VM_DIR}" || true
setfacl -R -d -m u:libvirt-qemu:rwx "${VM_DIR}" || true

ISO="${VM_DIR}/${ISO_NAME}"
if [[ ! -f "${ISO}" ]]; then
  echo "Downloading ${ISO_URL}"
  curl -L --fail -o "${ISO}.partial" "${ISO_URL}"
  mv "${ISO}.partial" "${ISO}"
fi

DISK="${VM_DIR}/archstreamer-client.qcow2"
if [[ ! -f "${DISK}" ]]; then
  qemu-img create -f qcow2 "${DISK}" 40G
fi

if ! virsh dominfo archstreamer-client >/dev/null 2>&1; then
  virt-install \
    --connect qemu:///system \
    --name archstreamer-client \
    --memory 8192 \
    --vcpus 4 \
    --cpu host \
    --disk path="${DISK}",format=qcow2,bus=virtio \
    --cdrom "${ISO}" \
    --os-variant ubuntu24.04 \
    --network network=default,model=virtio \
    --graphics spice,listen=none \
    --video qxl \
    --channel spicevmc \
    --input tablet,bus=usb \
    --sound ich9 \
    --boot uefi \
    --noautoconsole \
    --check path_in_use=off
else
  echo "Domain archstreamer-client already defined."
  virsh start archstreamer-client 2>/dev/null || true
fi

cat <<EOF

Host setup done.

- VM disk/ISO: ${VM_DIR}
- Open console (as ${REAL_USER}):
    sg libvirt -c 'virt-viewer --connect qemu:///system --attach archstreamer-client'
  or: virt-manager
- Guest reaches this metal host at: 192.168.122.1
- Log out/in once if libvirt/kvm group was just added.

EOF
