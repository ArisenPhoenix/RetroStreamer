# Second "computer" for LAN testing without a second physical PC.
# Host stays on the metal machine; this container is the remote client.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsdl2-2.0-0 \
    libpulse0 \
    libasound2t64 \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/archstreamer
# Binaries are bind-mounted at runtime from the host build dir.
CMD ["./session_client", "--help"]
