#!/usr/bin/env bash
# Last-resort DS4 passthrough: add a USB 2.0 EHCI controller and attach the pad there.
# qemu-xhci hostdev of DualShock 4 is failing on this host with guest
# "xhci WARN Set TR Deq Ptr cmd failed" / error -71.
#
# Usage:
#   1. Fully Shut Off the VM in Virt Manager
#   2. sudo ./fix-vm-usb-ehci-ds4.sh
#   3. Start the VM
#   4. Guest: lsusb | grep 054c
set -euo pipefail

VM="${1:-archstreamer-client}"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run with sudo." >&2
  exit 1
fi

STATE="$(virsh domstate "$VM")"
if [[ "$STATE" != "shut off" && "$STATE" != "shutoff" ]]; then
  echo "VM must be Shut Off (state=$STATE). Shut it down fully, then re-run." >&2
  exit 2
fi

if ! lsusb -d 054c:09cc >/dev/null; then
  echo "Plug the DS4 into a rear motherboard USB port first." >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Strip existing USB hostdevs from config.
virsh dumpxml "$VM" >"$TMP/domain.xml"
python3 - "$TMP" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
xml = (root / "domain.xml").read_text()
blocks = re.findall(r"<hostdev\b.*?</hostdev>", xml, flags=re.DOTALL)
for i, block in enumerate(blocks):
    (root / f"old-hostdev-{i}.xml").write_text(block + "\n")
print(len(blocks))
PY
shopt -s nullglob
for f in "$TMP"/old-hostdev-*.xml; do
  virsh detach-device "$VM" "$f" --config 2>/dev/null || true
done

# Ensure an EHCI+UHCI USB2 companion stack exists (index 1).
if ! virsh dumpxml "$VM" | grep -q "model='ich9-ehci1'"; then
  cat >"$TMP/ehci.xml" <<'XML'
<controller type='usb' index='1' model='ich9-ehci1'>
  <address type='pci' domain='0x0000' bus='0x00' slot='0x1d' function='0x7'/>
</controller>
XML
  cat >"$TMP/uhci1.xml" <<'XML'
<controller type='usb' index='1' model='ich9-uhci1'>
  <master startport='0'/>
  <address type='pci' domain='0x0000' bus='0x00' slot='0x1d' function='0x0' multifunction='on'/>
</controller>
XML
  cat >"$TMP/uhci2.xml" <<'XML'
<controller type='usb' index='1' model='ich9-uhci2'>
  <master startport='2'/>
  <address type='pci' domain='0x0000' bus='0x00' slot='0x1d' function='0x1'/>
</controller>
XML
  cat >"$TMP/uhci3.xml" <<'XML'
<controller type='usb' index='1' model='ich9-uhci3'>
  <master startport='4'/>
  <address type='pci' domain='0x0000' bus='0x00' slot='0x1d' function='0x2'/>
</controller>
XML
  echo "Adding USB2 EHCI/UHCI controllers..."
  # edit domain via attach-device --config for controllers is awkward; use virt-xml if present
  if command -v virt-xml >/dev/null; then
    virt-xml "$VM" --add-device --controller usb,model=ich9-ehci1,index=1
    virt-xml "$VM" --add-device --controller usb,model=ich9-uhci1,index=1
    virt-xml "$VM" --add-device --controller usb,model=ich9-uhci2,index=1
    virt-xml "$VM" --add-device --controller usb,model=ich9-uhci3,index=1
  else
    echo "virt-xml not installed. Install with: sudo apt install virtinst" >&2
    echo "Or in Virt Manager: Add Hardware → Controller → USB → USB EHCI" >&2
    exit 3
  fi
fi

# Unbind host drivers.
SYS=""
for d in /sys/bus/usb/devices/*; do
  [[ -f "$d/idVendor" ]] || continue
  if [[ "$(cat "$d/idVendor")" == "054c" && "$(cat "$d/idProduct")" == "09cc" ]]; then
    SYS="$d"
    break
  fi
done
echo "DS4 at $SYS"
for iface in "$SYS":*; do
  [[ -e "$iface/driver" ]] || continue
  drv="$(basename "$(readlink "$iface/driver")")"
  name="$(basename "$iface")"
  echo "Unbind $name from $drv"
  echo -n "$name" >"/sys/bus/usb/drivers/$drv/unbind" 2>/dev/null || true
done

# Attach to guest USB bus 1 (EHCI) explicitly when possible.
cat >"$TMP/ds4.xml" <<'XML'
<hostdev mode='subsystem' type='usb' managed='yes'>
  <source>
    <vendor id='0x054c'/>
    <product id='0x09cc'/>
  </source>
  <address type='usb' bus='1' port='1'/>
</hostdev>
XML

virsh attach-device "$VM" "$TMP/ds4.xml" --config
echo "OK. Start the VM, then on guest: lsusb | grep -i 054c"
echo
echo "If this still fails, skip USB passthrough for now:"
echo "  - Play as Host Player on the metal host (controller works there), or"
echo "  - In virt-viewer: Virtual Machine → Redirect USB Device → DualShock 4"
