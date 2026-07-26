#include "client/keyboard_poller.hpp"

#include "client/remoted_keyboard_source.hpp"
#include "common/time.hpp"

#include <memory>

namespace archstreamer {

struct KeyboardPoller::Impl {
    std::unique_ptr<RemotedKeyboardSource> source;
};

KeyboardPoller::KeyboardPoller() {
    impl_ = new Impl();
    impl_->source = make_default_remoted_keyboard_source();
}

KeyboardPoller::~KeyboardPoller() {
    delete impl_;
    impl_ = nullptr;
}

std::string KeyboardPoller::backend_status() const {
    if (impl_ == nullptr || impl_->source == nullptr) {
        return "Remoted keyboard: unavailable";
    }
    return std::string("Remoted keyboard: ") + impl_->source->status_detail();
}

std::optional<KeyboardState> KeyboardPoller::poll() {
    if (impl_ == nullptr || impl_->source == nullptr) {
        return std::nullopt;
    }

    KeyboardState state;
    state.sequence = ++sequence_;
    state.timestamp_us = steady_timestamp_us();
    state.keys = impl_->source->poll_keys();
    return state;
}

} // namespace archstreamer
