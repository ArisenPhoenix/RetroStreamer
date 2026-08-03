#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace archstreamer {

/**
 * Dual same-host Ryujinx LDN needs distinct IPs/ports. Firejail --net=<bridge>
 * puts each slot in its own netns on a shared L2 bridge so UDP 12345 / 11452
 * no longer collide.
 *
 * Requires: firejail with networking enabled (restricted-network no), and the
 * libvirt network "archstreamer-ldn" (bridge asldnbr0).
 */
bool ldn_firejail_available();

/** Ensure libvirt network archstreamer-ldn is defined/started. */
bool ensure_ldn_bridge();

/**
 * Argv prefix: firejail --noprofile --net=asldnbr0 --ip=172.31.200.(10+slot) …
 * Empty if firejail/bridge unavailable (caller should log and continue).
 */
std::vector<std::string> ldn_firejail_command_prefix(std::size_t slot_index);

/** Guest-side interface name inside the firejail netns (for Ryujinx LAN iface). */
std::string ldn_guest_interface_name();

/** IPv4 assigned to the slot inside the netns. */
std::string ldn_slot_ipv4(std::size_t slot_index);

} // namespace archstreamer
