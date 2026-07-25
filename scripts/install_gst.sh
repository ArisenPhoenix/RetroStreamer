#!/usr/bin/env bash
# Linux GStreamer runtime for ArchStreamer client receive + host capture/encode.
# Host Switch/Yuzu also needs gamescope (+ Gamescope WSI); that is not apt GStreamer.
set -euo pipefail
sudo apt update
sudo apt install -y \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  gstreamer1.0-x \
  gstreamer1.0-pulseaudio \
  gstreamer1.0-pipewire \
  gstreamer1.0-plugins-bad
# Optional NVIDIA encode (host): nvh264enc lives in bad/nvcodec when the driver is present.