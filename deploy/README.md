# deploy

Deployment and installation tooling lives here.

This directory is for scripts that package ArchStreamer, install dependencies,
finish updates, or prepare target environments. Platform-specific packaging belongs
in the relevant child directory, such as `windows/` or `flatpak/`.

Runtime code should not depend on files in this directory.
