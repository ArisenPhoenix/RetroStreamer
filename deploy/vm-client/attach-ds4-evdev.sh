#!/usr/bin/env bash
# Pass a joystick into the guest as virtio-input / evdev, or return it to the metal host.
#
# USB hostdev of DualShock 4 (054c:09cc) fails on many hosts; use this instead.
#
# Attach (VM client play):
#   ./deploy/vm-client/attach-ds4-evdev.sh
#   ./deploy/vm-client/attach-ds4-evdev.sh archstreamer-client /dev/input/by-id/...-event-joystick
#
# Detach (metal Host Player / bare-metal client needs the pad):
#   ./deploy/vm-client/attach-ds4-evdev.sh detach
#   ./deploy/vm-client/attach-ds4-evdev.sh detach archstreamer-client
#
# While attached, QEMU usually grabs the evdev node exclusively — Host Player on
# the metal machine will see a dead controller until you detach.
set -euo pipefail

MODE=attach
VM=archstreamer-client
BYID=/dev/input/by-id/usb-Sony_Interactive_Entertainment_Wireless_Controller-if03-event-joystick

if [[ "${1:-}" == "detach" || "${1:-}" == "--detach" ]]; then
  MODE=detach
  shift
  VM="${1:-archstreamer-client}"
elif [[ "${1:-}" == "attach" || "${1:-}" == "--attach" ]]; then
  shift
  VM="${1:-archstreamer-client}"
  BYID="${2:-$BYID}"
else
  VM="${1:-archstreamer-client}"
  BYID="${2:-$BYID}"
fi

run() { sg libvirt -c "$*"; }

detach_passthrough() {
  run "virsh dumpxml $VM" | python3 -c "
import sys, re
xml = sys.stdin.read()
blocks = re.findall(r\"<input type='passthrough'.*?</input>\", xml, flags=re.S)
for i, block in enumerate(blocks):
    open(f'/tmp/ds4-evdev-detach-{i}.xml', 'w').write(block)
print(len(blocks))
" || true
  local n=0
  shopt -s nullglob
  for f in /tmp/ds4-evdev-detach-*.xml; do
    run "virsh detach-device $VM $f --live" 2>/dev/null || true
    run "virsh detach-device $VM $f --config" 2>/dev/null || true
    n=$((n + 1))
  done
  rm -f /tmp/ds4-evdev-detach-*.xml
  echo "Detached $n virtio-evdev passthrough device(s) from $VM."
  echo "Pad is available to metal Host Player / bare-metal ArchStreamer client."
}

if [[ "$MODE" == "detach" ]]; then
  detach_passthrough
  exit 0
fi

if [[ ! -e "$BYID" ]]; then
  echo "Joystick evdev not found: $BYID" >&2
  echo "Plug the pad into the metal host, then: ls /dev/input/by-id/" >&2
  exit 1
fi

# Replace any existing passthrough first.
detach_passthrough >/dev/null || true

cat > /tmp/ds4-evdev.xml <<XML
<input type='passthrough' bus='virtio'>
  <source evdev='$BYID'/>
</input>
XML

run "virsh attach-device $VM /tmp/ds4-evdev.xml --live --config"
echo "OK. Attached $BYID -> $VM"
echo "Guest: grep -A5 Wireless /proc/bus/input/devices"
echo "Metal Host Player will NOT receive this pad until: $0 detach"
