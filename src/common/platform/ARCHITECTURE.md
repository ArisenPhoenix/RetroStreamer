# src/common/platform Architecture

`src/common/platform/` implements common platform adapters.

CMake selects POSIX or Windows translation units for process control, sockets,
discovery network interfaces, path resolution, and process utilities.

Code outside this directory should call the common abstraction rather than
branching directly on OS APIs.
