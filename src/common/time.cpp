#include "common/time.hpp"

namespace archstreamer {

std::uint64_t steady_timestamp_us() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

} // namespace archstreamer
