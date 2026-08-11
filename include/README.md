# include

Public C++ headers live here.

Headers are grouped by target area:

- `client/`: desktop client APIs.
- `common/`: protocol, serialization, data types, and shared helpers.
- `host/`: host-side APIs and subsystem boundaries.
- `tools/`: command-line tool entry points and wrappers.

Prefer including headers by their installed-style path, such as
`host/session/runtime.hpp`, rather than by relative filesystem paths.
