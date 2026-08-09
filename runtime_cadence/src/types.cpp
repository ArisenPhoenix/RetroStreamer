#include "archstreamer/runtime_cadence/types.hpp"

#include "common/sha256.hpp"

#include <algorithm>
#include <cctype>
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

std::string canonical_identity_name(std::string_view name) {
    std::string out(name);
    const auto first = std::find_if_not(out.begin(), out.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(out.rbegin(), out.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    out = std::string(first, last);
    return out;
}

std::string identity_id_from_name(std::string_view name) {
    const auto canonical = canonical_identity_name(name);
    if (canonical.empty()) {
        return {};
    }
    auto digest = sha256_hex("archstreamer-identity-v1:" + canonical);
    constexpr std::string_view prefix = "sha256:";
    if (digest.rfind(prefix, 0) == 0) {
        digest.erase(0, prefix.size());
    }
    return "identity-v1-" + digest.substr(0, 32);
}

} // namespace archstreamer::cadence
