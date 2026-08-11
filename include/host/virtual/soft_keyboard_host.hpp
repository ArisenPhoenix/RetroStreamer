#pragma once

#include "common/protocol.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace archstreamer {

/**
 * Shared between the Ryujinx Software Keyboard watcher thread and
 * SessionControlMonitor (outbound request / inbound pad-OSK response).
 */
struct SoftKeyboardHostBridge {
    std::mutex mutex;
    std::optional<SoftKeyboardRequest> pending_to_clients;
    bool pending_to_clients_sent = false;
    std::optional<SoftKeyboardResponse> pending_from_client;
    // request_id == 0: client opened the pad OSK manually and wants the host to
    // find any open text dialog and type this in (escape hatch for missed prompts).
    std::optional<std::string> pending_manual_inject;
    std::uint32_t next_request_id = 1;

    SoftKeyboardRequest make_request(
        std::string prompt,
        std::string initial_text,
        std::uint8_t max_length) {
        SoftKeyboardRequest request;
        request.request_id = next_request_id++;
        if (next_request_id == 0) {
            next_request_id = 1;
        }
        request.prompt = std::move(prompt);
        request.initial_text = std::move(initial_text);
        request.max_length = max_length == 0 ? std::uint8_t{12} : max_length;
        return request;
    }

    void publish_request(SoftKeyboardRequest request) {
        std::lock_guard lock(mutex);
        pending_from_client.reset();
        pending_to_clients = std::move(request);
        pending_to_clients_sent = false;
    }

    std::optional<SoftKeyboardRequest> take_unsent_request() {
        std::lock_guard lock(mutex);
        if (!pending_to_clients.has_value() || pending_to_clients_sent) {
            return std::nullopt;
        }
        pending_to_clients_sent = true;
        return pending_to_clients;
    }

    void submit_response(SoftKeyboardResponse response) {
        std::lock_guard lock(mutex);
        if (response.request_id == 0) {
            if (response.accepted && !response.text.empty()) {
                pending_manual_inject = std::move(response.text);
            }
            return;
        }
        pending_from_client = std::move(response);
    }

    std::optional<SoftKeyboardResponse> take_response() {
        std::lock_guard lock(mutex);
        auto response = pending_from_client;
        pending_from_client.reset();
        return response;
    }

    std::optional<std::string> take_manual_inject() {
        std::lock_guard lock(mutex);
        auto text = pending_manual_inject;
        pending_manual_inject.reset();
        return text;
    }

    void clear() {
        std::lock_guard lock(mutex);
        pending_to_clients.reset();
        pending_to_clients_sent = false;
        pending_from_client.reset();
        pending_manual_inject.reset();
    }
};

} // namespace archstreamer
