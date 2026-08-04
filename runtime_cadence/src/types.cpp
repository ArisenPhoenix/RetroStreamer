#include "archstreamer/runtime_cadence/types.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace archstreamer::cadence {

std::int64_t now_epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string day_string_from_epoch(std::int64_t epoch_seconds) {
    if (epoch_seconds <= 0) {
        epoch_seconds = now_epoch_seconds();
    }
    const std::time_t t = static_cast<std::time_t>(epoch_seconds);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d");
    return out.str();
}

} // namespace archstreamer::cadence
