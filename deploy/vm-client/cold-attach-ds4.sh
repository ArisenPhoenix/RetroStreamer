#!/usr/bin/env bash
# Cold-attach DualShock 4 for reliable VM passthrough.
#
# Hot-add through the ASMedia front-panel hub (5-5.x) often fails with guest
# "device descriptor read/64, error -71". Prefer a rear motherboard USB port,
# then shut down the VM and run this before starting it again.
#
# Usage: sudo ./cold-attach-ds4.sh [vm-name]
set -euo pipefail

VM="${1:-archstreamer-client}"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run with sudo." >&2
  exit 1
fi

if ! lsusb -d 054c:09cc >/dev/null; then
  echo "Plug the DS4 into the host first (prefer a REAR motherboard USB port)." >&2
  exit 1
fi

SYS=""
for d in /sys/bus/usb/devices/*; do
  [[ -f "$d/idVendor" ]] || continue
  if [[ "$(cat "$d/idVendor")" == "054c" && "$(cat "$d/idProduct")" == "09cc" ]]; then
    SYS="$d"
    break
  fi
done

echo "DS4 sysfs: $SYS"
echo "USB path:  $(readlink -f "$SYS")"
lsusb -d 054c:09cc
lsusb -t | sed -n '1,120p' | grep -n . | head -5 >/dev/null

if [[ "$SYS" == *"/5-5."* ]] || [[ "$(readlink -f "$SYS")" == *"/usb5/5-5/"* ]]; then
  cat <<'WARN' >&2

*** WARNING ***
The DS4 is on the ASMedia ASM1074 hub (typical front-panel / case USB).
Passthrough from that hub is what has been failing with guest error -71.

Unplug it and plug into a REAR motherboard USB port (not a hub), then re-run:
  sudo ./cold-attach-ds4.sh

WARN
  # Continue anyway if user insists, but warn clearly.
fi

STATE="$(virsh domstate "$VM")"
echo "VM state: $STATE"
if [[ "$STATE" != "shut off" && "$STATE" != "shutoff" ]]; then
  cat <<EOF >&2

VM must be fully shut off for a clean cold attach (hot-add has been unreliable).
In Virt Manager: Shut Down the guest, wait until state is Shutoff, then re-run:

  sudo $0 $VM

EOF
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Remove every existing USB hostdev from persistent config.
virsh dumpxml "$VM" >"$TMP/domain.xml"
python3 - "$TMP" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
xml = (root / "domain.xml").read_text()
blocks = re.findall(r"<hostdev\b.*?</hostdev>", xml, flags=re.DOTALL)
for i, block in enumerate(blocks):
    (root / f"hostdev-{i}.xml").write_text(block + "\n")
print(f"found {len(blocks)} hostdev block(s)")
PY

shopt -s nullglob
for f in "$TMP"/hostdev-*.xml; do
  echo "Removing $(basename "$f") from config..."
  virsh detach-device "$VM" "$f" --config 2>/dev/null || true
done

# Unbind host drivers so start-up claim is clean.
for iface in "$SYS":*; do
  [[ -e "$iface/driver" ]] || continue
  drv="$(basename "$(readlink "$iface/driver")")"
  name="$(basename "$iface")"
  echo "Unbind $name from $drv"
  echo -n "$name" >"/sys/bus/usb/drivers/$drv/unbind" 2>/dev/null || true
done

cat >"$TMP/ds4.xml" <<'XML'
<hostdev mode='subsystem' type='usb' managed='yes'>
  <source>
    <vendor id='0x054c'/>
    <product id='0x09cc'/>
  </source>
</hostdev>
XML

echo "Adding DS4 (054c:09cc) to domain config..."
virsh attach-device "$VM" "$TMP/ds4.xml" --config

echo
echo "Config hostdevs:"
virsh dumpxml "$VM" | grep -E 'hostdev|vendor|product' | head -20
echo
echo "Next:"
echo "  1. Start the VM in Virt Manager"
echo "  2. On guest: lsusb | grep -i 054c"
echo "  3. Refresh Controllers in ArchStreamer"
