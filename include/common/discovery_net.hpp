#pragma once

#include <string>
#include <vector>

namespace archstreamer {

/** Broadcast destinations for LAN discovery (255.255.255.255, loopback, iface broadcasts). */
std::vector<std::string> ipv4_broadcast_targets();

} // namespace archstreamer
