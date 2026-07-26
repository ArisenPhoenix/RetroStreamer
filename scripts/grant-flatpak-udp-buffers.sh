#!/usr/bin/env bash
# Raise kernel UDP buffer limits so the ArchStreamer Flatpak can set a large
# SO_RCVBUF without CAP_NET_ADMIN (Flatpak does not expose a "net.admin" toggle).
#
# Why: GStreamer udpsrc buffer-size=… calls setsockopt(SO_RCVBUF). Values above
# net.core.rmem_max fail with EPERM inside the sandbox ("Need net.admin privilege?").
# On Wi‑Fi that leaves scene-cut keyframes easy to drop.
#
# Usage (on the CLIENT machine, e.g. Bazzite):
#   sudo ./scripts/grant-flatpak-udp-buffers.sh
#   # then restart: flatpak run io.github.ArisenPhoenix.ArchStreamer
#
# Undo:
#   sudo ./scripts/grant-flatpak-udp-buffers.sh --revoke
set -euo pipefail

APP_ID="io.github.ArisenPhoenix.ArchStreamer"
SYSCTL_FILE="/etc/sysctl.d/99-archstreamer-udp.conf"
# Kernel doubles SO_RCVBUF requests; 8 MiB max allows ~4 MiB effective app requests.
RMEM_MAX="${ARCHSTREAMER_RMEM_MAX:-8388608}"
RMEM_DEFAULT="${ARCHSTREAMER_RMEM_DEFAULT:-1048576}"
# What ArchStreamer requests via udpsrc (must be <= rmem_max/2 in practice).
UDP_RCVBUF="${ARCHSTREAMER_UDP_RCVBUF:-2097152}"

REVOKE=0
if [[ "${1:-}" == "--revoke" || "${1:-}" == "revoke" ]]; then
  REVOKE=1
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run with sudo: sudo $0${1:+ $1}" >&2
  exit 1
fi

# Prefer the installing user for --user Flatpak overrides (sudo otherwise targets root).
FLATPAK_USER="${SUDO_USER:-${USER}}"
if [[ -z "${FLATPAK_USER}" || "${FLATPAK_USER}" == "root" ]]; then
  FLATPAK_USER="$(logname 2>/dev/null || true)"
fi

run_as_user() {
  if [[ -n "${FLATPAK_USER}" && "${FLATPAK_USER}" != "root" ]] && command -v runuser >/dev/null 2>&1; then
    runuser -u "${FLATPAK_USER}" -- "$@"
  elif [[ -n "${FLATPAK_USER}" && "${FLATPAK_USER}" != "root" ]]; then
    sudo -u "${FLATPAK_USER}" -- "$@"
  else
    "$@"
  fi
}

if [[ "${REVOKE}" -eq 1 ]]; then
  echo "Revoking ArchStreamer UDP buffer allowances…"
  if [[ -f "${SYSCTL_FILE}" ]]; then
    rm -f "${SYSCTL_FILE}"
    echo "  removed ${SYSCTL_FILE}"
  else
    echo "  no ${SYSCTL_FILE} to remove"
  fi
  if command -v flatpak >/dev/null 2>&1; then
    run_as_user flatpak override --user --unset-env=ARCHSTREAMER_UDP_RCVBUF "${APP_ID}" 2>/dev/null || true
    echo "  cleared Flatpak env ARCHSTREAMER_UDP_RCVBUF for ${APP_ID} (user=${FLATPAK_USER:-root})"
  fi
  if command -v sysctl >/dev/null 2>&1; then
    sysctl --system >/dev/null 2>&1 || true
  fi
  echo "Done. Restart ArchStreamer if it is running."
  exit 0
fi

echo "ArchStreamer Flatpak UDP buffer grant"
echo "  Note: Flatpak cannot grant CAP_NET_ADMIN to the app."
echo "  This raises net.core.rmem_* so SO_RCVBUF succeeds without that capability."
echo "  rmem_max=${RMEM_MAX}  rmem_default=${RMEM_DEFAULT}  app buffer-size=${UDP_RCVBUF}"
echo

cat > "${SYSCTL_FILE}" <<EOF
# ArchStreamer — allow large UDP receive buffers for Flatpak RTP video
# (avoids GStreamer "Need net.admin privilege?" on udpsrc buffer-size=…)
net.core.rmem_max = ${RMEM_MAX}
net.core.rmem_default = ${RMEM_DEFAULT}
net.core.wmem_max = ${RMEM_MAX}
net.core.wmem_default = ${RMEM_DEFAULT}
EOF
echo "  wrote ${SYSCTL_FILE}"

if command -v sysctl >/dev/null 2>&1; then
  sysctl -w "net.core.rmem_max=${RMEM_MAX}" >/dev/null
  sysctl -w "net.core.rmem_default=${RMEM_DEFAULT}" >/dev/null
  sysctl -w "net.core.wmem_max=${RMEM_MAX}" >/dev/null
  sysctl -w "net.core.wmem_default=${RMEM_DEFAULT}" >/dev/null
  echo "  applied sysctl for current boot"
else
  echo "  warning: sysctl not found; reboot to apply ${SYSCTL_FILE}" >&2
fi

if command -v flatpak >/dev/null 2>&1; then
  if run_as_user flatpak info --user "${APP_ID}" >/dev/null 2>&1 ||
     flatpak info --user "${APP_ID}" >/dev/null 2>&1 ||
     flatpak info "${APP_ID}" >/dev/null 2>&1; then
    run_as_user flatpak override --user \
      --env="ARCHSTREAMER_UDP_RCVBUF=${UDP_RCVBUF}" \
      "${APP_ID}"
    echo "  Flatpak override: ARCHSTREAMER_UDP_RCVBUF=${UDP_RCVBUF} (user=${FLATPAK_USER:-root})"
  else
    echo "  note: ${APP_ID} not installed yet; sysctl alone is enough once you install the Flatpak."
    echo "  after install you can re-run this script, or:"
    echo "    flatpak override --user --env=ARCHSTREAMER_UDP_RCVBUF=${UDP_RCVBUF} ${APP_ID}"
  fi
else
  echo "  note: flatpak not on PATH; sysctl limits still help a native client."
fi

echo
echo "Done. Restart ArchStreamer (flatpak run ${APP_ID})."
echo "Revoke later with: sudo $0 --revoke"
