# Android protocol Package Architecture

`protocol/` owns Android packet models and codecs.

This package mirrors the shared native protocol vocabulary closely. Any protocol
change should be checked against `include/common/protocol.hpp` and the C++
serialization layer.
