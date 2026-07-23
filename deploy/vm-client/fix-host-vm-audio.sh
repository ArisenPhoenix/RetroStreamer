#!/usr/bin/env bash
# Fix ArchStreamer client VM audio by playing guest sound into the host
# PipeWire/Pulse session (HDMI), bypassing broken SPICE playback.
#
# Run on the metal host:
#   pkexec bash deploy/vm-client/fix-host-vm-audio.sh
set -euo pipefail

PROFILE=/etc/apparmor.d/libvirt/libvirt-e90c64d1-d551-4d8a-a8ce-3b035cc056b7
XML=/tmp/archstreamer-client-audio.xml
RUNTIME_USER=1000
RUNTIME_DIR=/run/user/${RUNTIME_USER}

if [[ ! -f "$PROFILE" ]]; then
  echo "missing apparmor profile: $PROFILE" >&2
  exit 1
fi

echo "==> AppArmor: allow QEMU to use host Pulse/PipeWire"
python3 - "$PROFILE" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
text = p.read_text()
needle = "#include <libvirt/libvirt-e90c64d1-d551-4d8a-a8ce-3b035cc056b7.files>"
extra = """
  # ArchStreamer: system QEMU -> host PipeWire/Pulse
  /run/user/1000/ r,
  /run/user/1000/pulse/ rw,
  /run/user/1000/pulse/** rw,
  /run/user/1000/pipewire-0 rw,
  /run/user/1000/pipewire-0-manager rw,
  /usr/share/pipewire/** r,
  /usr/share/spa-*/** r,
  /usr/lib/*/spa-*/** mr,
  /usr/lib/*/pipewire-*/** mr,
"""
if "/run/user/1000/pulse" in text and "/run/user/1000/ r" in text:
    print("apparmor already patched")
elif "/run/user/1000/pulse" in text:
    # older patch without parent dir read — refresh block
    import re
    text = re.sub(
        r"\n  # ArchStreamer:.*?(?=\n}\n?\\Z|\n  #include|\n})",
        "\n" + extra,
        text,
        count=1,
        flags=re.S,
    )
    if "/run/user/1000/ r" not in text:
        text = text.replace(needle, needle + "\n" + extra, 1)
    p.write_text(text)
    print("apparmor profile updated")
else:
    if needle not in text:
        raise SystemExit(f"needle missing in {p}")
    p.write_text(text.replace(needle, needle + "\n" + extra, 1))
    print("apparmor profile patched")
PY
apparmor_parser -r "$PROFILE"
echo "apparmor reloaded"

echo "==> ACL: let libvirt-qemu traverse ${RUNTIME_DIR} (mode 700 otherwise blocks the socket)"
if [[ ! -d "${RUNTIME_DIR}/pulse" ]]; then
  echo "Pulse runtime missing at ${RUNTIME_DIR}/pulse — is user ${RUNTIME_USER} logged into a graphical session?" >&2
  exit 1
fi
# Need execute on each path component to reach the socket; rw on pulse files.
setfacl -m u:libvirt-qemu:--x /run/user || true
setfacl -m u:libvirt-qemu:--x "${RUNTIME_DIR}"
setfacl -m u:libvirt-qemu:rwx "${RUNTIME_DIR}/pulse"
setfacl -m u:libvirt-qemu:rw "${RUNTIME_DIR}/pulse/native" || true
setfacl -m u:libvirt-qemu:r "${RUNTIME_DIR}/pulse/pid" || true
# PipeWire sockets if present
[[ -S "${RUNTIME_DIR}/pipewire-0" ]] && setfacl -m u:libvirt-qemu:rw "${RUNTIME_DIR}/pipewire-0" || true
[[ -S "${RUNTIME_DIR}/pipewire-0-manager" ]] && setfacl -m u:libvirt-qemu:rw "${RUNTIME_DIR}/pipewire-0-manager" || true
getfacl -p "${RUNTIME_DIR}" | sed -n '1,20p'
getfacl -p "${RUNTIME_DIR}/pulse" | sed -n '1,20p'

echo "==> Domain XML: pulseaudio backend + env"
virsh -c qemu:///system dumpxml archstreamer-client > "$XML"
python3 - "$XML" <<'PY'
from pathlib import Path
import re
import sys
p = Path(sys.argv[1])
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

env_block = """  <qemu:commandline>
    <qemu:env name='XDG_RUNTIME_DIR' value='/run/user/1000'/>
    <qemu:env name='PULSE_SERVER' value='unix:/run/user/1000/pulse/native'/>
    <qemu:env name='PIPEWIRE_RUNTIME_DIR' value='/run/user/1000'/>
  </qemu:commandline>
"""
text = text.replace("</domain>", env_block + "</domain>", 1)
p.write_text(text)
print("domain xml updated")
PY

virsh -c qemu:///system destroy archstreamer-client 2>/dev/null || true
virsh -c qemu:///system define "$XML"
virsh -c qemu:///system start archstreamer-client
echo "==> VM started"
virsh -c qemu:///system dumpxml archstreamer-client | rg -n "audio|qemu:env|xmlns:qemu"

cat <<'EOF'

Done.

Guest audio should now appear on the host as a QEMU/Pulse stream (not Virt Viewer).
After the guest desktop is up:
  1. Play a tone / YouTube in the VM
  2. On the host: pactl list short sink-inputs

If this machine reboots, re-run the ACL section (or this whole script) after login —
/run/user/1000 is recreated each session.

EOF
