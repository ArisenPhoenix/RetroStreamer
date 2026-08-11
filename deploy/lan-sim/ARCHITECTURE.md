# deploy/lan-sim Architecture

`deploy/lan-sim/` owns the containerized LAN simulation environment.

It is for smoke testing network behavior with separate IP stacks and repeatable
logs. It should depend on built binaries rather than becoming a runtime library.
