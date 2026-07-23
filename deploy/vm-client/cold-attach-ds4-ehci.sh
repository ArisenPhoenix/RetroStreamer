#!/usr/bin/env bash
# Attach DS4 to the guest USB2 EHCI bus (index 1). VM must be running AFTER
# the EHCI controllers were added to domain config (requires one full restart).
set -euo pipefail
VM="${1:-archstreamer-client}"
run() { sg libvirt -c "$*"; }

if ! run "virsh dumpxml $VM" | grep -q "ich9-ehci1"; then
  echo "EHCI not present in LIVE domain. Shut Off the VM fully, start it again, then re-run." >&2
  exit 2
fi

# Drop any existing usb hostdev
run "virsh dumpxml $VM" | python3 -c "
import sys,re
m=re.search(r\"<hostdev mode='subsystem' type='usb'.*?</hostdev>\", sys.stdin.read(), re.S)
open('/tmp/ds4-detach.xml','w').write(m.group(0) if m else '')
"
if [[ -s /tmp/ds4-detach.xml ]]; then
  run "virsh detach-device $VM /tmp/ds4-detach.xml --live" || true
  run "virsh detach-device $VM /tmp/ds4-detach.xml --config" || true
  sleep 1
fi

if ! lsusb -d 054c:09cc >/dev/null; then
  echo "DS4 not visible on host (054c:09cc). Plug it into a rear USB port." >&2
  exit 1
fi

cat > /tmp/ds4-ehci.xml <<'XML'
<hostdev mode='subsystem' type='usb' managed='yes'>
  <source>
    <vendor id='0x054c'/>
    <product id='0x09cc'/>
  </source>
  <address type='usb' bus='1' port='1'/>
</hostdev>
XML

run "virsh attach-device $VM /tmp/ds4-ehci.xml --live --config"
sleep 2
echo "Host lsusb:"; lsusb -d 054c:09cc || echo "(not listed — often OK if qemu claimed it)"
echo "QEMU usb:"; run "virsh qemu-monitor-command $VM --hmp 'info usb'"
echo
echo "On guest check: lsusb | grep -i 054c"
echo "If still empty, use virt-viewer: Virtual Machine → Redirect USB Device → DualShock 4"
