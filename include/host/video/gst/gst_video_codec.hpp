#pragma once

#include "host/video/gst/gst_video_command.hpp"

#include <iosfwd>

namespace archstreamer {

/**
 * Named-field length-prefixed protocol.
 *
 *   <verb>\n
 *   <field_count>\n
 *   <name>\n
 *   <len>\n
 *   <bytes>   (repeated)
 *
 * Event:
 *   <kind>\n
 *   <message_len>\n
 *   <message bytes>
 */
bool write_gst_video_command(std::ostream& out, const GstVideoCommand& command);
bool read_gst_video_command(std::istream& in, GstVideoCommand& command);

bool write_gst_video_event(std::ostream& out, const GstVideoEvent& event);
bool read_gst_video_event(std::istream& in, GstVideoEvent& event);

} // namespace archstreamer
