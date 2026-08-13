#!/usr/bin/env bash
# Example remote Ensure Host start script (Path B).
#
# Client Remote tab: set "Start script" to this file's absolute path on the host.
# ArchStreamer SSH-invokes it with ports + optional GPU only, e.g.:
#   /path/to/remote_host_start_example.sh \
#     --control-port 45555 --input-port 45454 --video-port 5004 --audio-port 6004 \
#     --virtual-display :99 [--gpu 'nvidia:0']
#
# The script owns host_runner location and any sanitize/setup. The config file owns
# stable host_runner defaults such as ROM/save/log paths.
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
default_host_config="${ARCHSTREAMER_HOST_CONFIG:-$HOME/archstreamer-host.conf}"
# -----------------------------------------

control_port=""
input_port=""
video_port=""
audio_port=""
virtual_display=""
gpu=""
host_config="$default_host_config"
extra_args=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --control-port) control_port="${2:-}"; shift 2 ;;
    --input-port) input_port="${2:-}"; shift 2 ;;
    --video-port) video_port="${2:-}"; shift 2 ;;
    --audio-port) audio_port="${2:-}"; shift 2 ;;
    --virtual-display) virtual_display="${2:-}"; shift 2 ;;
    --gpu) gpu="${2:-}"; shift 2 ;;
    --config) host_config="${2:-}"; shift 2 ;;
    --) shift; extra_args+=("$@"); break ;;
    *)
      extra_args+=("$1")
      shift
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
if [[ ! -r "$host_config" ]]; then
  echo "host config missing or unreadable: $host_config" >&2
  exit 1
fi

# Optional: sanitize environment, fix PATH, claim GPU, etc. before exec.
# Example: export PATH="/usr/local/bin:$PATH"

args=(
  --config "$host_config"
  --control-port "$control_port"
  --input-port "$input_port"
  --video-port "$video_port"
  --audio-port "$audio_port"
  --virtual-display "$virtual_display"
)
if [[ -n "$gpu" ]]; then
  args+=(--gpu "$gpu")
fi
args+=("${extra_args[@]}")

exec "$HOST_RUNNER" "${args[@]}"
