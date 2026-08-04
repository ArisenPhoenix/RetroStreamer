#!/usr/bin/env bash
# Example remote Ensure Host start script (Path B).
#
# Client Remote tab: set "Start script" to this file's absolute path on the host.
# ArchStreamer SSH-invokes it with ports + optional GPU only, e.g.:
#   /path/to/remote_host_start_example.sh \
#     --control-port 45555 --input-port 45454 --video-port 5004 --audio-port 6004 \
#     --virtual-display :99 [--gpu 'nvidia:0']
#
# The script owns ROM root, host_runner location, and any sanitize/setup.
# Use `exec` so the PID ArchStreamer tracks is host_runner (Stop Host / pkill work).
#
# Permissions (e.g. user alina):
#   - chmod +x this script; readable/executable by the SSH user
#   - SSH user can run host_runner and reach devices it needs
#     (GPU /renderD*, uinput, gamescope, Pulse, display as your setup requires)
#   - Prefer installing under a path that user owns, or group+ACL if shared

set -euo pipefail

# --- edit these for the remote machine ---
HOST_RUNNER="${HOST_RUNNER:-/home/alina/ArchStreamer/build/host_runner}"
ROM_ROOT="${ROM_ROOT:-/home/alina/roms}"
# -----------------------------------------

control_port=""
input_port=""
video_port=""
audio_port=""
virtual_display=""
gpu=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --control-port) control_port="${2:-}"; shift 2 ;;
    --input-port) input_port="${2:-}"; shift 2 ;;
    --video-port) video_port="${2:-}"; shift 2 ;;
    --audio-port) audio_port="${2:-}"; shift 2 ;;
    --virtual-display) virtual_display="${2:-}"; shift 2 ;;
    --gpu) gpu="${2:-}"; shift 2 ;;
    *)
      echo "unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$control_port" || -z "$input_port" || -z "$video_port" || -z "$audio_port" || -z "$virtual_display" ]]; then
  echo "missing required port/display args from Ensure Host" >&2
  exit 2
fi

if [[ ! -x "$HOST_RUNNER" ]]; then
  echo "host_runner not executable: $HOST_RUNNER" >&2
  exit 127
fi

# Optional: sanitize environment, fix PATH, claim GPU, etc. before exec.
# Example: export PATH="/usr/local/bin:$PATH"

args=(
  --rom-root "$ROM_ROOT"
  --control-port "$control_port"
  --input-port "$input_port"
  --video-port "$video_port"
  --audio-port "$audio_port"
  --virtual-display "$virtual_display"
  --clients 2
  --allow-new-users
)
if [[ -n "$gpu" ]]; then
  args+=(--gpu "$gpu")
fi

exec "$HOST_RUNNER" "${args[@]}"
