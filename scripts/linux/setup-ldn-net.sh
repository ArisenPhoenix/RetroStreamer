#!/usr/bin/env bash
# One-shot host setup for dual-Ryujinx Local Wireless (LDN) on one machine.
set -euo pipefail

echo "== Firejail networking =="
if grep -q '^restricted-network yes' /etc/firejail/firejail.config 2>/dev/null; then
  echo "Enabling firejail networking for regular users (needs admin)…"
  pkexec bash -c 'sed -i "s/^restricted-network yes/restricted-network no/" /etc/firejail/firejail.config; sed -i "s/^# network yes/network yes/" /etc/firejail/firejail.config'
fi
grep -E '^(network|restricted-network) ' /etc/firejail/firejail.config || true

echo "== Libvirt LDN bridge (asldnbr0 / 172.31.200.0/24) =="
if ! virsh net-info archstreamer-ldn >/dev/null 2>&1; then
  tmp=$(mktemp)
  cat >"$tmp" <<'EOF'
<network>
  <name>archstreamer-ldn</name>
  <bridge name='asldnbr0' stp='off' delay='0'/>
  <ip address='172.31.200.1' netmask='255.255.255.0'/>
</network>
EOF
  virsh net-define "$tmp"
  rm -f "$tmp"
fi
virsh net-autostart archstreamer-ldn
virsh net-start archstreamer-ldn || true
ip -br link show asldnbr0 || true

echo "Done. Restart host_runner; each Switch slot gets firejail --net=asldnbr0 with a unique IP."
