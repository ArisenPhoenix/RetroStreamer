# include/client Architecture

`include/client/` defines the desktop client's reusable runtime API.

The client side owns:

- connecting to a host over the control channel;
- maintaining catalog/art cache state;
- capturing local controller and keyboard input;
- sending input packets to the host;
- receiving and presenting host media.

The desktop GUI and CLI should use these interfaces rather than rebuilding the
session flow. Shared wire types and normalized controls belong in
`include/common/`.
