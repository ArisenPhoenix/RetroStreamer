#!/usr/bin/env bash
# Make the ArchStreamer client VM start with working host-heard audio.
#
# Default: SPICE audio (plays through virt-manager / virt-viewer).
# This is the reliable path — system QEMU does not need to punch into your
# user PipeWire session.
#
# Optional: --host-pulse routes QEMU into the logged-in Pulse/PipeWire session
# (more fragile: AppArmor + ACLs; re-run after reboot/login).
#
#   sudo ./deploy/vm-client/fix-host-vm-audio.sh
#   sudo ./deploy/vm-client/fix-host-vm-audio.sh --host-pulse
#   sudo ./deploy/vm-client/fix-host-vm-audio.sh archstreamer-client 1000 --host-pulse
set -euo pipefail

VM="archstreamer-client"
RUNTIME_USER="${SUDO_UID:-$(id -u)}"
MODE="spice"
ARGS=()
for arg in "$@"; do
  case "$arg" in
    --host-pulse) MODE="pulse" ;;
    --spice) MODE="spice" ;;
    *) ARGS+=("$arg") ;;
  esac
done
if [[ ${#ARGS[@]} -ge 1 ]]; then
  VM="${ARGS[0]}"
fi
if [[ ${#ARGS[@]} -ge 2 ]]; then
  RUNTIME_USER="${ARGS[1]}"
fi

RUNTIME_DIR="/run/user/${RUNTIME_USER}"
XML="/tmp/${VM}-audio.xml"
URI="qemu:///system"

UUID="$(virsh -c "$URI" domuuid "$VM" 2>/dev/null || true)"
if [[ -z "$UUID" ]]; then
  echo "Domain not found: $VM (need virsh access to qemu:///system)" >&2
  exit 1
fi

PROFILE="/etc/apparmor.d/libvirt/libvirt-${UUID}"
FILES="${PROFILE}.files"

define_spice_audio() {
  echo "==> Domain XML: SPICE audio (heard in virt-manager / virt-viewer)"
  virsh -c "$URI" dumpxml "$VM" > "$XML"
  python3 - "$XML" <<'PY'
from pathlib import Path
import re
import sys

p = Path(sys.argv[1])
text = p.read_text()
text, n = re.subn(
    r"<audio id='1' type='[^']*'(?:[^<>]*)?/>",
    "<audio id='1' type='spice'/>",
    text,
    count=1,
)
if n == 0:
    text = text.replace(
        "</devices>",
        "    <audio id='1' type='spice'/>\n  </devices>",
        1,
    )
text = re.sub(r"\s*<qemu:commandline>.*?</qemu:commandline>\s*", "\n", text, flags=re.S)
if "qemu:" not in text:
    text = text.replace(" xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'", "", 1)
p.write_text(text)
print(f"audio -> spice (replacements={max(n, 1)})")
PY
}

wait_for_apparmor_pair() {
  local seconds="${1:-30}"
  local i
  for i in $(seq 1 "$((seconds * 10))"); do
    if [[ -f "$PROFILE" && -f "$FILES" ]]; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

apply_runtime_acls() {
  echo "==> ACL: let libvirt-qemu use ${RUNTIME_DIR} Pulse/PipeWire"
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
}

patch_apparmor_for_host_audio() {
  if [[ -f "$PROFILE" && ! -f "$FILES" ]]; then
    echo "==> Incomplete AppArmor pair — removing profile so libvirt can recreate"
    apparmor_parser -R "$PROFILE" 2>/dev/null || true
    rm -f "$PROFILE"
  fi

  if [[ ! -f "$PROFILE" || ! -f "$FILES" ]]; then
    echo "==> Bootstrapping AppArmor pair with a short SPICE start"
    define_spice_audio
    virsh -c "$URI" destroy "$VM" 2>/dev/null || true
    virsh -c "$URI" define "$XML"
    virsh -c "$URI" start "$VM"
    if ! wait_for_apparmor_pair 30; then
      echo "libvirt did not create ${PROFILE} + ${FILES}" >&2
      exit 1
    fi
    virsh -c "$URI" destroy "$VM" 2>/dev/null || true
  fi

  echo "==> AppArmor: allow QEMU -> host Pulse (uid ${RUNTIME_USER})"
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
marker = f"/run/user/{uid}/pulse/**"
text = re.sub(
    r"\n  # ArchStreamer: system QEMU -> host PipeWire/Pulse\n(?:  .*\n)*",
    "\n",
    text,
)
if needle not in text:
    raise SystemExit(f"needle missing in {p}: {needle}")
if marker in text:
    print("apparmor already patched")
else:
    p.write_text(text.replace(needle, needle + "\n" + extra, 1))
    print("apparmor profile patched")
PY
  apparmor_parser -r "$PROFILE"
  echo "apparmor reloaded"
}

define_pulse_audio() {
  echo "==> Domain XML: host PulseAudio backend"
  virsh -c "$URI" dumpxml "$VM" > "$XML"
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
if n == 0:
    text = text.replace(
        "</devices>",
        "    <audio id='1' type='pulseaudio'/>\n  </devices>",
        1,
    )
print(f"audio replacements: {max(n, 1)}")

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
}

virsh -c "$URI" destroy "$VM" 2>/dev/null || true

if [[ "$MODE" == "spice" ]]; then
  define_spice_audio
  virsh -c "$URI" define "$XML"
  virsh -c "$URI" start "$VM"
  echo "==> VM started: $VM (audio=spice)"
  cat <<EOF

Guest sound should play through the virt-manager / virt-viewer window.
If silent: View → unmute / raise volume in the console viewer.

Your host speakers already work; QEMU does not need direct Pulse access for this mode.
EOF
else
  apply_runtime_acls
  patch_apparmor_for_host_audio
  define_pulse_audio
  virsh -c "$URI" define "$XML"
  virsh -c "$URI" start "$VM"
  echo "==> VM started: $VM (audio=pulseaudio into uid ${RUNTIME_USER})"
  cat <<EOF

Guest audio should appear on the host as a QEMU/Pulse stream.
  pactl list short sink-inputs

Re-run after reboot/login (ACLs on ${RUNTIME_DIR} are per-session).
EOF
fi

virsh -c "$URI" dumpxml "$VM" | grep -E "audio|qemu:env" || true
