#!/usr/bin/env bash
# Apply ArchStreamer LAN firewall allowances.
# Requires root: sudo ./scripts/apply-firewall.sh
#
# Run on BOTH machines when testing across a LAN:
#   - Host: control TCP, input UDP, discovery UDP (clients connect inbound here)
#   - Client: video/audio UDP (host sends RTP inbound to the client)
# Opening only the host is not enough for picture/sound on the remote client.
set -euo pipefail

CONTROL_TCP="${ARCHSTREAMER_CONTROL_PORT:-45555}"
INPUT_UDP="${ARCHSTREAMER_INPUT_PORT:-45454}"
DISCOVERY_UDP="${ARCHSTREAMER_DISCOVERY_PORT:-45550}"
VIDEO_UDP_START="${ARCHSTREAMER_VIDEO_PORT:-5004}"
AUDIO_UDP_START="${ARCHSTREAMER_AUDIO_PORT:-6004}"
MEDIA_SPAN="${ARCHSTREAMER_MEDIA_PORT_SPAN:-8}"

VIDEO_UDP_END=$((VIDEO_UDP_START + MEDIA_SPAN - 1))
AUDIO_UDP_END=$((AUDIO_UDP_START + MEDIA_SPAN - 1))

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run with sudo: sudo $0" >&2
  exit 1
fi

echo "ArchStreamer firewall allowances:"
echo "  TCP  ${CONTROL_TCP}            (session control — host)"
echo "  UDP  ${INPUT_UDP}              (controller input — host)"
echo "  UDP  ${DISCOVERY_UDP}          (LAN host discovery — host)"
echo "  UDP  ${VIDEO_UDP_START}-${VIDEO_UDP_END}   (RTP video — CLIENT inbound)"
echo "  UDP  ${AUDIO_UDP_START}-${AUDIO_UDP_END}   (RTP audio — CLIENT inbound)"
echo
echo "Tip: on Bazzite/Steam Deck clients, these media UDP ports are required for a video window."
echo

if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
  firewall-cmd --permanent --add-port="${CONTROL_TCP}/tcp"
  firewall-cmd --permanent --add-port="${INPUT_UDP}/udp"
  firewall-cmd --permanent --add-port="${DISCOVERY_UDP}/udp"
  firewall-cmd --permanent --add-port="${VIDEO_UDP_START}-${VIDEO_UDP_END}/udp"
  firewall-cmd --permanent --add-port="${AUDIO_UDP_START}-${AUDIO_UDP_END}/udp"
  firewall-cmd --reload
  echo "Done (firewalld). Bazzite/Fedora: ports opened permanently."
  exit 0
fi

if command -v ufw >/dev/null 2>&1; then
  ufw allow "${CONTROL_TCP}/tcp" comment 'ArchStreamer control'
  ufw allow "${INPUT_UDP}/udp" comment 'ArchStreamer input'
  ufw allow "${DISCOVERY_UDP}/udp" comment 'ArchStreamer discovery'
  ufw allow "${VIDEO_UDP_START}:${VIDEO_UDP_END}/udp" comment 'ArchStreamer video RTP'
  ufw allow "${AUDIO_UDP_START}:${AUDIO_UDP_END}/udp" comment 'ArchStreamer audio RTP'
  ufw status numbered | sed -n '1,40p'
  echo
  echo "If UFW was inactive, enable with: sudo ufw enable"
  echo "Done (ufw)."
  exit 0
fi

if command -v nft >/dev/null 2>&1; then
  nft list tables | grep -q 'inet archstreamer' && nft delete table inet archstreamer || true
  nft add table inet archstreamer
  nft add chain inet archstreamer input '{ type filter hook input priority 0; policy accept; }'
  nft add rule inet archstreamer input tcp dport "${CONTROL_TCP}" accept comment \"ArchStreamer control\"
  nft add rule inet archstreamer input udp dport "${INPUT_UDP}" accept comment \"ArchStreamer input\"
  nft add rule inet archstreamer input udp dport "${DISCOVERY_UDP}" accept comment \"ArchStreamer discovery\"
  nft add rule inet archstreamer input udp dport "${VIDEO_UDP_START}"-"${VIDEO_UDP_END}" accept comment \"ArchStreamer video\"
  nft add rule inet archstreamer input udp dport "${AUDIO_UDP_START}"-"${AUDIO_UDP_END}" accept comment \"ArchStreamer audio\"
  nft list table inet archstreamer
  echo "Done (nftables table inet archstreamer)."
  exit 0
fi

echo "Neither firewalld, ufw, nor nft found." >&2
echo "Manually open: TCP ${CONTROL_TCP}, UDP ${INPUT_UDP}, UDP ${DISCOVERY_UDP}," >&2
echo "  UDP ${VIDEO_UDP_START}-${VIDEO_UDP_END}, UDP ${AUDIO_UDP_START}-${AUDIO_UDP_END}" >&2
exit 1
