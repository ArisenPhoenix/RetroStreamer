#!/usr/bin/env bash
# Pass a USB headset (default: Samsung USBC Headset 04e8:a051) into the guest,
# or return it to the metal host.
#
# Goal: metal keeps HDMI (or other) for Host / Watch-local; guest owns the
# earbuds so host and client audio are on separate physical paths.
#
# Attach:
#   ./deploy/vm-client/attach-usb-headset.sh
#   ./deploy/vm-client/attach-usb-headset.sh archstreamer-client 04e8:a051
#
# Detach (earbuds back on metal):
#   ./deploy/vm-client/attach-usb-headset.sh detach
#   ./deploy/vm-client/attach-usb-headset.sh detach archstreamer-client
set -euo pipefail

MODE=attach
VM=archstreamer-client
VIDPID=04e8:a051

if [[ "${1:-}" == "detach" || "${1:-}" == "--detach" ]]; then
  MODE=detach
  shift
  VM="${1:-archstreamer-client}"
  VIDPID="${2:-$VIDPID}"
elif [[ "${1:-}" == "attach" || "${1:-}" == "--attach" ]]; then
  shift
  VM="${1:-archstreamer-client}"
  VIDPID="${2:-$VIDPID}"
else
  VM="${1:-archstreamer-client}"
  VIDPID="${2:-$VIDPID}"
fi

if [[ ! "$VIDPID" =~ ^([0-9a-fA-F]{4}):([0-9a-fA-F]{4})$ ]]; then
  echo "VID:PID must look like 04e8:a051 (got: $VIDPID)" >&2
  exit 1
fi
VID="${BASH_REMATCH[1],,}"
PID="${BASH_REMATCH[2],,}"

run() { sg libvirt -c "$*"; }

hostdev_xml() {
  cat <<XML
<hostdev mode='subsystem' type='usb' managed='yes'>
  <source>
    <vendor id='0x${VID}'/>
    <product id='0x${PID}'/>
  </source>
</hostdev>
XML
}

detach_matching() {
  run "virsh dumpxml $VM" | VID="$VID" PID="$PID" python3 -c "
import os, re, sys
vid = os.environ['VID'].lower()
pid = os.environ['PID'].lower()
xml = sys.stdin.read()
blocks = re.findall(r\"<hostdev mode='subsystem' type='usb'.*?</hostdev>\", xml, flags=re.S)
n = 0
for block in blocks:
    if re.search(rf\"vendor id='0x{vid}'\", block, re.I) and re.search(rf\"product id='0x{pid}'\", block, re.I):
        path = f'/tmp/usb-headset-detach-{n}.xml'
        open(path, 'w').write(block)
        n += 1
print(n)
"
  local n=0
  shopt -s nullglob
  for f in /tmp/usb-headset-detach-*.xml; do
    run "virsh detach-device $VM $f --live" 2>/dev/null || true
    run "virsh detach-device $VM $f --config" 2>/dev/null || true
    n=$((n + 1))
  done
  rm -f /tmp/usb-headset-detach-*.xml
  echo "Detached $n USB hostdev(s) for ${VID}:${PID} from $VM."
}

metal_prefer_hdmi() {
  if ! command -v pactl >/dev/null 2>&1; then
    return 0
  fi
  local hdmi
  hdmi="$(pactl list short sinks 2>/dev/null | awk '/hdmi/ { print $2; exit }' || true)"
  if [[ -n "${hdmi}" ]]; then
    pactl set-default-sink "$hdmi" 2>/dev/null || true
    echo "Metal default sink -> $hdmi"
  fi
}

if [[ "$MODE" == "detach" ]]; then
  detach_matching
  echo "Earbuds should reappear on metal Pulse/PipeWire shortly."
  exit 0
fi

if ! lsusb | grep -qi "${VID}:${PID}"; then
  echo "USB device ${VID}:${PID} not found on metal. Plug it in first." >&2
  lsusb >&2 || true
  exit 1
fi

# Prefer HDMI on metal before stealing the headset from PipeWire.
metal_prefer_hdmi

detach_matching >/dev/null || true
hostdev_xml > /tmp/usb-headset-attach.xml
run "virsh attach-device $VM /tmp/usb-headset-attach.xml --live --config"
echo "OK. Attached USB ${VID}:${PID} -> $VM (persistent + live)."
echo "Guest: lsusb | grep -i ${VID}:${PID}"
echo "Guest Settings → Audio output should list the headset; metal uses HDMI."
echo "Return earbuds to metal: $0 detach"
