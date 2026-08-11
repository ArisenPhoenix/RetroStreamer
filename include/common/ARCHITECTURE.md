# include/common Architecture

`include/common/` is the common vocabulary shared by host, desktop client,
Android native integrations, tools, and tests.

It owns protocol packets, serialization, discovery data, controller
normalization, keyboard state, catalog identity, pairing, address helpers,
hashing, and portable time/path-oriented data where that data is not host- or
client-specific.

This layer must avoid depending on host session state, Qt UI code, or platform
device implementations.
