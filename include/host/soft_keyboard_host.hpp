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
        pending_from_client = std::move(response);
    }

    std::optional<SoftKeyboardResponse> take_response() {
        std::lock_guard lock(mutex);
        auto response = pending_from_client;
        pending_from_client.reset();
        return response;
    }

    void clear() {
        std::lock_guard lock(mutex);
        pending_to_clients.reset();
        pending_to_clients_sent = false;
        pending_from_client.reset();
    }
};

} // namespace archstreamer
