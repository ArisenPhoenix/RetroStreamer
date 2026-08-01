#include "client/audio_playback_device.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace archstreamer {
namespace {

std::mutex g_preferred_mutex;
std::string g_preferred_audio_output = "auto";
std::atomic<std::uint64_t> g_preferred_epoch{1};

} // namespace

void set_preferred_audio_output_device(std::string id) {
    if (id.empty()) {
        id = "auto";
    }
    {
        std::lock_guard lock(g_preferred_mutex);
        if (g_preferred_audio_output == id) {
            return;
        }
        g_preferred_audio_output = std::move(id);
    }
    g_preferred_epoch.fetch_add(1, std::memory_order_relaxed);
}

std::string preferred_audio_output_device() {
    std::lock_guard lock(g_preferred_mutex);
    return g_preferred_audio_output;
}

std::uint64_t audio_output_preference_epoch() {
    return g_preferred_epoch.load(std::memory_order_relaxed);
}

} // namespace archstreamer
