#!/usr/bin/env bash
# Fix ArchStreamer client VM audio by playing guest sound into the host
# PipeWire/Pulse session (HDMI), bypassing broken SPICE playback.
#
# Run on the metal host (as root):
#   sudo ./deploy/vm-client/fix-host-vm-audio.sh
#   sudo ./deploy/vm-client/fix-host-vm-audio.sh archstreamer-client 1000
set -euo pipefail

VM="${1:-archstreamer-client}"
RUNTIME_USER="${2:-${SUDO_UID:-$(id -u)}}"
RUNTIME_DIR="/run/user/${RUNTIME_USER}"
XML="/tmp/${VM}-audio.xml"

UUID="$(virsh -c qemu:///system domuuid "$VM" 2>/dev/null || true)"
if [[ -z "$UUID" ]]; then
  echo "Domain not found: $VM (need virsh access to qemu:///system)" >&2
  exit 1
fi

PROFILE="/etc/apparmor.d/libvirt/libvirt-${UUID}"
if [[ ! -f "$PROFILE" ]]; then
  echo "missing apparmor profile: $PROFILE" >&2
  echo "(start the VM once so libvirt creates it, then re-run)" >&2
  exit 1
fi

echo "==> AppArmor: allow QEMU to use host Pulse/PipeWire (uid ${RUNTIME_USER})"
python3 - "$PROFILE" "$UUID" "$RUNTIME_USER" <<'PY'
from pathlib import Path
import re
import sys

p = Path(sys.argv[1])
uuid = sys.argv[2]
uid = sys.argv[3]
text = p.read_text()
needle = f"#include <libvirt/libvirt-{uuid}.files>"
extra = f"""
  # ArchStreamer: system QEMU -> host PipeWire/Pulse
  /run/user/{uid}/ r,
  /run/user/{uid}/pulse/ rw,
  /run/user/{uid}/pulse/** rw,
  /run/user/{uid}/pipewire-0 rw,
  /run/user/{uid}/pipewire-0-manager rw,
  /usr/share/pipewire/** r,
  /usr/share/spa-*/** r,
  /usr/lib/*/spa-*/** mr,
  /usr/lib/*/pipewire-*/** mr,
"""
marker = f"/run/user/{uid}/pulse"
parent = f"/run/user/{uid}/ r"
if marker in text and parent in text:
    print("apparmor already patched")
elif marker in text:
    text = re.sub(
        r"\n  # ArchStreamer:.*?(?=\n}\n?\\Z|\n  #include|\n})",
        "\n" + extra,
        text,
        count=1,
        flags=re.S,
    )
    if parent not in text:
        text = text.replace(needle, needle + "\n" + extra, 1)
    p.write_text(text)
    print("apparmor profile updated")
else:
    if needle not in text:
        raise SystemExit(f"needle missing in {p}: {needle}")
    p.write_text(text.replace(needle, needle + "\n" + extra, 1))
    print("apparmor profile patched")
PY
apparmor_parser -r "$PROFILE"
echo "apparmor reloaded"

echo "==> ACL: let libvirt-qemu traverse ${RUNTIME_DIR}"
if [[ ! -d "${RUNTIME_DIR}/pulse" ]]; then
  echo "Pulse runtime missing at ${RUNTIME_DIR}/pulse — is uid ${RUNTIME_USER} in a graphical session?" >&2
  exit 1
fi
setfacl -m u:libvirt-qemu:--x /run/user || true
setfacl -m u:libvirt-qemu:--x "${RUNTIME_DIR}"
setfacl -m u:libvirt-qemu:rwx "${RUNTIME_DIR}/pulse"
setfacl -m u:libvirt-qemu:rw "${RUNTIME_DIR}/pulse/native" || true
setfacl -m u:libvirt-qemu:r "${RUNTIME_DIR}/pulse/pid" || true
[[ -S "${RUNTIME_DIR}/pipewire-0" ]] && setfacl -m u:libvirt-qemu:rw "${RUNTIME_DIR}/pipewire-0" || true
[[ -S "${RUNTIME_DIR}/pipewire-0-manager" ]] && setfacl -m u:libvirt-qemu:rw "${RUNTIME_DIR}/pipewire-0-manager" || true

echo "==> Domain XML: pulseaudio backend + env"
virsh -c qemu:///system dumpxml "$VM" > "$XML"
python3 - "$XML" "$RUNTIME_USER" <<'PY'
from pathlib import Path
import re
import sys

p = Path(sys.argv[1])
uid = sys.argv[2]
text = p.read_text()

if "xmlns:qemu=" not in text:
    text = text.replace(
        "<domain type='kvm'",
        "<domain type='kvm' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'",
        1,
    )

text, n = re.subn(
    r"<audio id='1' type='[^']*'(?:[^<>]*)?/>",
    "<audio id='1' type='pulseaudio'/>",
    text,
    count=1,
)
print(f"audio replacements: {n}")

text = re.sub(r"<qemu:commandline>.*?</qemu:commandline>\s*", "", text, flags=re.S)

env_block = f"""  <qemu:commandline>
    <qemu:env name='XDG_RUNTIME_DIR' value='/run/user/{uid}'/>
    <qemu:env name='PULSE_SERVER' value='unix:/run/user/{uid}/pulse/native'/>
    <qemu:env name='PIPEWIRE_RUNTIME_DIR' value='/run/user/{uid}'/>
  </qemu:commandline>
"""
text = text.replace("</domain>", env_block + "</domain>", 1)
p.write_text(text)
print("domain xml updated")
PY

virsh -c qemu:///system destroy "$VM" 2>/dev/null || true
virsh -c qemu:///system define "$XML"
virsh -c qemu:///system start "$VM"
echo "==> VM started: $VM"
virsh -c qemu:///system dumpxml "$VM" | grep -E "audio|qemu:env|xmlns:qemu" || true

cat <<EOF

Done.

Guest audio should appear on the host as a QEMU/Pulse stream (not Virt Viewer).
After the guest desktop is up:
  1. Play a tone / YouTube in the VM
  2. On the host: pactl list short sink-inputs

After host reboot, re-run the ACL section (or this whole script) after login —
${RUNTIME_DIR} is recreated each session.
EOF
