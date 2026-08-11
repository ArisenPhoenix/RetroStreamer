#pragma once

#include "archstreamer/runtime_cadence/types.hpp"

#include <string>
#include <string_view>

namespace archstreamer {

/** Process id string used as cadence host_id (matches host_started). */
std::string cadence_host_id();

/** Soft-fail record into the build-selected RuntimeStore. */
void record_cadence_event(cadence::RuntimeEvent event);

void record_session_started(
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view detail = {},
    std::string_view session_id = {});

void record_session_ended(
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view detail = {},
    std::string_view session_id = {});

void record_client_joined(
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view detail = {},
    std::string_view session_id = {});

void record_client_left(
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view detail = {},
    std::string_view session_id = {});

} // namespace archstreamer
