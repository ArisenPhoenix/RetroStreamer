# include/common/platform Architecture

`include/common/platform/` defines the portable common platform layer.

It owns contracts for child processes, sockets, process utilities, and path
helpers. Public headers select or expose platform-specific implementations while
keeping downstream code on stable common types.

Product/session policy should not live here. This layer should still make sense
as generic process, socket, and filesystem support.
