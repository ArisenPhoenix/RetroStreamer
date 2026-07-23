#!/usr/bin/env bash
# Run inside the Ubuntu guest to diagnose why VM audio is silent.
set -euo pipefail

echo "==> Session / audio server"
echo "DISPLAY=${DISPLAY:-<unset>} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-<unset>} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-<unset>}"
systemctl --user --no-pager --full status pipewire pipewire-pulse wireplumber 2>/dev/null | sed -n '1,40p' || true
pactl info 2>/dev/null || echo "pactl info failed (PipeWire/Pulse not reachable)"

echo
echo "==> Cards / sinks / default"
aplay -l 2>/dev/null || echo "aplay -l failed (no ALSA cards?)"
pactl list short cards 2>/dev/null || true
pactl list short sinks 2>/dev/null || true
pactl get-default-sink 2>/dev/null || true
pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null || true
pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null || true

echo
echo "==> 2s tone via Pulse (should hear in virt-viewer if SPICE audio works)"
if command -v paplay >/dev/null && [[ -f /usr/share/sounds/alsa/Front_Center.wav ]]; then
  paplay /usr/share/sounds/alsa/Front_Center.wav || true
elif command -v speaker-test >/dev/null; then
  speaker-test -c 2 -t sine -f 440 -l 1 >/dev/null 2>&1 || true
else
  echo "No paplay/speaker-test available"
fi

echo
echo "==> GStreamer autoaudiosink (ArchStreamer path)"
if command -v gst-launch-1.0 >/dev/null; then
  timeout 3 gst-launch-1.0 -q audiotestsrc num-buffers=50 ! audioconvert ! audioresample ! autoaudiosink \
    && echo "gst autoaudiosink: OK (process exited cleanly)" \
    || echo "gst autoaudiosink: FAILED (exit $?)"
  gst-inspect-1.0 pulsesink >/dev/null 2>&1 && echo "pulsesink: installed" || echo "pulsesink: MISSING (apt install gstreamer1.0-pulseaudio)"
  gst-inspect-1.0 pipewiresink >/dev/null 2>&1 && echo "pipewiresink: installed" || echo "pipewiresink: not installed (optional)"
else
  echo "gst-launch-1.0 missing"
fi

echo
echo "==> Next checks on the metal (outside the guest)"
echo "1. Use virt-viewer/virt-manager console (not SSH-only)."
echo "2. In virt-viewer: View → unmute / raise volume; some builds mute guest audio by default."
echo "3. Metal: pactl list short sink-inputs | while playing tone in guest — you should see virt-viewer/spicy."
echo "Done."
