# deploy/windows Architecture

`deploy/windows/` owns Windows install, update, dependency, and build helpers.

This directory handles PowerShell/bash wrappers and update finishing behavior.
Windows runtime abstractions belong in `src/common/platform/`, `src/client/`, or
`src/host/hardware/`.
