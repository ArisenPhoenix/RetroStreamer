# deploy Architecture

`deploy/` owns packaging, installation, and update flows.

Deployment scripts should consume built artifacts and repository configuration.
They should not own runtime behavior. Platform-specific installation details
belong in child directories such as `windows/`, `flatpak/`, and `vm-client/`.
