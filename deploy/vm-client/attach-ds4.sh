#!/usr/bin/env bash
# Force-reattach DualShock 4 to archstreamer-client by vendor/product.
# Fixes stale bus/device attaches (e.g. /dev/bus/usb/005/015) that cause guest error -71.
# Usage: sudo ./attach-ds4.sh [vm-name]
set -euo pipefail

VM="${1:-archstreamer-client}"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run with sudo." >&2
  exit 1
fi

if ! lsusb -d 054c:09cc >/dev/null; then
  echo "No DS4 (054c:09cc) on the host. Plug it in first." >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== Current USB hostdevs in $VM =="
virsh dumpxml "$VM" | awk '
  /<hostdev / {grab=1; buf=$0; next}
  grab {buf=buf ORS $0}
  grab && /<\/hostdev>/ {
    print "-----"
    print buf
    n++
    grab=0; buf=""
  }
  END { if (!n) print "(none)" }
'

# Write each hostdev block to its own file and detach live + config.
virsh dumpxml "$VM" >"$TMP/domain.xml"
python3 - "$TMP" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
xml = (root / "domain.xml").read_text()
blocks = re.findall(r"<hostdev\b.*?</hostdev>", xml, flags=re.DOTALL)
for i, block in enumerate(blocks):
    (root / f"hostdev-{i}.xml").write_text(block + "\n")
print(len(blocks))
PY

shopt -s nullglob
for f in "$TMP"/hostdev-*.xml; do
  echo "Detaching $(basename "$f") (live)..."
  virsh detach-device "$VM" "$f" --live 2>/dev/null || true
  echo "Detaching $(basename "$f") (config)..."
  virsh detach-device "$VM" "$f" --config 2>/dev/null || true
done

# Unbind host drivers.
while read -r sys; do
  [[ -z "$sys" ]] && continue
  echo "Unbinding interfaces under $sys"
  for iface in "$sys":*; do
    [[ -e "$iface/driver" ]] || continue
    drv="$(basename "$(readlink "$iface/driver")")"
    name="$(basename "$iface")"
    echo "  $name <- $drv"
    echo -n "$name" >"/sys/bus/usb/drivers/$drv/unbind" 2>/dev/null || true
  done
  # USB device reset helps clear error -71 state.
  if [[ -e "$sys/authorized" ]]; then
    echo "USB reset via authorized toggle"
    echo 0 >"$sys/authorized" || true
    sleep 0.5
    echo 1 >"$sys/authorized" || true
    sleep 1
  fi
done < <(
  for d in /sys/bus/usb/devices/*; do
    [[ -f "$d/idVendor" ]] || continue
    [[ "$(cat "$d/idVendor")" == "054c" && "$(cat "$d/idProduct")" == "09cc" ]] && printf '%s\n' "$d"
  done
)

# Wait for device to reappear after reset.
for _ in $(seq 1 20); do
  lsusb -d 054c:09cc >/dev/null && break
  sleep 0.25
done
if ! lsusb -d 054c:09cc >/dev/null; then
  echo "DS4 did not come back after reset. Unplug/replug, then re-run." >&2
  exit 1
fi
lsusb -d 054c:09cc

# Unbind again post-reset (kernel will rebind quickly).
while read -r sys; do
  for iface in "$sys":*; do
    [[ -e "$iface/driver" ]] || continue
    drv="$(basename "$(readlink "$iface/driver")")"
    name="$(basename "$iface")"
    echo -n "$name" >"/sys/bus/usb/drivers/$drv/unbind" 2>/dev/null || true
  done
done < <(
  for d in /sys/bus/usb/devices/*; do
    [[ -f "$d/idVendor" ]] || continue
    [[ "$(cat "$d/idVendor")" == "054c" && "$(cat "$d/idProduct")" == "09cc" ]] && printf '%s\n' "$d"
  done
)

cat >"$TMP/ds4.xml" <<'XML'
<hostdev mode='subsystem' type='usb' managed='yes'>
  <source>
    <vendor id='0x054c'/>
    <product id='0x09cc'/>
  </source>
</hostdev>
XML

echo "Attaching by vendor/product..."
virsh attach-device "$VM" "$TMP/ds4.xml" --live

if virsh attach-device "$VM" "$TMP/ds4.xml" --config; then
  echo "Persisted in domain config."
else
  echo "Note: already present in domain config (OK)."
fi

echo
echo "Hostdevs now:"
virsh dumpxml "$VM" | grep -E 'hostdev|vendor|product|address bus' | head -40
echo
echo "On the guest:  lsusb | grep -i 054c"
echo "If still empty, fully shut down the VM (not reboot), run this script again, then start the VM."
