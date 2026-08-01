#include "common/addresses.hpp"
#include "common/platform/paths.hpp"
#include "common/platform/process_utils.hpp"
#include "host/gstreamer_media_server.hpp"
#include "host/host_launch_planner.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include <unistd.h>

namespace archstreamer {
namespace {

std::string multiudp_clients_arg(
    const std::vector<std::pair<std::string, std::uint16_t>>& clients) {
    std::string joined;
    for (const auto& [host, port] : clients) {
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += host;
        joined.push_back(':');
        joined += std::to_string(port);
    }
    return joined;
}

bool gst_element_available(const char* element) {
    if (!command_available("gst-inspect-1.0")) {
        return false;
    }
    return std::system(
               (std::string("gst-inspect-1.0 ") + element + " >/dev/null 2>&1").c_str()) == 0;
}

} // namespace

GStreamerVideoFanout::~GStreamerVideoFanout() {
    stop();
}

GStreamerVideoFanout::Destination* GStreamerVideoFanout::find_destination(ClientId client_id) {
    for (auto& destination : destinations_) {
        if (destination.client_id == client_id) {
            return &destination;
        }
    }
    return nullptr;
}

const GStreamerVideoFanout::Destination* GStreamerVideoFanout::find_destination(
    ClientId client_id) const {
    for (const auto& destination : destinations_) {
        if (destination.client_id == client_id) {
            return &destination;
        }
    }
    return nullptr;
}

std::string GStreamerVideoFanout::staging_encode_log_path() {
    const auto directory = std::filesystem::path{archstreamer_cache_directory()};
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return (directory / "gst-video-staging-encode.log").string();
}

void GStreamerVideoFanout::apply_nvenc_environment(
    ChildProcess& process,
    std::vector<std::string> args,
    const std::optional<std::string>& stderr_path) {
    process.start(
        std::move(args),
        nvenc_cuda_device_id_ >= 0
            ? std::vector<std::pair<std::string, std::string>>{
                  {"CUDA_DEVICE_ORDER", "PCI_BUS_ID"},
                  {"CUDA_VISIBLE_DEVICES", std::to_string(nvenc_cuda_device_id_)},
              }
            : std::vector<std::pair<std::string, std::string>>{},
        nvenc_cuda_device_id_ >= 0
            ? std::vector<std::string>{
                  "__NV_PRIME_RENDER_OFFLOAD",
                  "__NV_PRIME_RENDER_OFFLOAD_PROVIDER",
                  "__GLX_VENDOR_LIBRARY_NAME",
                  "DRI_PRIME",
              }
            : std::vector<std::string>{},
        stderr_path);
}

std::vector<std::string> GStreamerVideoFanout::build_single_encode_args(
    MediaQualityTier tier,
    const std::string& host,
    std::uint16_t port) const {
    if (!gst_element_available("multiudpsink")) {
        throw std::runtime_error("multiudpsink is required for video encode (gst-plugins-good)");
    }
    if (source_kind_ == SourceKind::PipeWire && !gst_element_available("pipewiresrc")) {
        throw std::runtime_error("pipewiresrc is required for gamescope video capture");
    }

    const auto settings = video_encode_settings_for_tier(tier);
    const int bitrate = settings.bitrate_kbps == 0 ? 1500 : settings.bitrate_kbps;
    const int framerate = settings.framerate == 0 ? 30 : static_cast<int>(settings.framerate);
    const int configured_key_int =
        settings.key_int_max == 0 ? framerate : static_cast<int>(settings.key_int_max);
    const int sixth_sec = std::max(5, framerate / 6);
    const int key_int_max = std::min(configured_key_int, sixth_sec);
    const bool nvenc = gst_element_available("nvh264enc");

    auto args = std::vector<std::string>{"gst-launch-1.0", "-q"};
    if (source_kind_ == SourceKind::PipeWire) {
        args.insert(args.end(), {
            "pipewiresrc",
            "path=" + pipewire_node_,
            "do-timestamp=true",
            "!",
            "video/x-raw,format=BGRx",
            "!",
            "videoconvert",
            "!",
        });
    } else {
        args.insert(args.end(), {
            "ximagesrc",
            "display-name=" + display_,
            "use-damage=false",
            "show-pointer=false",
            "do-timestamp=true",
            "!",
            "videoconvert",
            "!",
        });
    }

    args.insert(args.end(), {
        "queue",
        "max-size-buffers=1",
        "max-size-time=0",
        "max-size-bytes=0",
        "leaky=upstream",
        "!",
    });
    if (settings.width > 0 && settings.height > 0) {
        args.insert(args.end(), {
            "videoscale",
            "method=0",
            "!",
            "video/x-raw,width=" + std::to_string(settings.width) +
                ",height=" + std::to_string(settings.height),
            "!",
        });
    }
    args.insert(args.end(), {
        "videorate",
        "drop-only=true",
        "!",
        "video/x-raw,framerate=" + std::to_string(framerate) + "/1",
        "!",
    });
    if (nvenc) {
        args.insert(args.end(), {
            "nvh264enc",
            "zerolatency=true",
            "preset=low-latency-hp",
            "strict-gop=true",
            "bitrate=" + std::to_string(bitrate),
            "gop-size=" + std::to_string(key_int_max),
            "!",
            "video/x-h264,profile=baseline,stream-format=byte-stream",
        });
    } else {
        args.insert(args.end(), {
            "x264enc",
            "tune=zerolatency",
            "speed-preset=ultrafast",
            "bitrate=" + std::to_string(bitrate),
            "key-int-max=" + std::to_string(key_int_max),
            "byte-stream=true",
            "bframes=0",
            "threads=1",
            "option-string=scenecut=40",
            "!",
            "video/x-h264,profile=constrained-baseline,stream-format=byte-stream",
        });
    }
    if (gst_element_available("h264parse")) {
        args.insert(args.end(), {"!", "h264parse", "config-interval=-1"});
    }
    args.insert(args.end(), {
        "!",
        "rtph264pay",
        "mtu=1200",
        "config-interval=-1",
        "aggregate-mode=zero-latency",
        "pt=96",
        "!",
        "multiudpsink",
        "clients=" + multiudp_clients_arg({{host, port}}),
        "sync=false",
        "async=false",
    });
    return args;
}

std::vector<MediaClientStream> GStreamerVideoFanout::start(
    const std::string& display,
    const std::vector<MediaStreamRequest>& destinations) {
    if (!destinations_.empty() || process_.running()) {
        throw std::runtime_error("video fanout is already running");
    }

    source_kind_ = SourceKind::X11;
    display_ = display;
    pipewire_node_.clear();
    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());
    for (const auto& destination : destinations) {
        Destination slot{};
        slot.client_id = destination.client_id;
        slot.host = destination.destination_host;
        slot.base_port = destination.port;
        slot.port = destination.port;
        slot.tier = MediaQualityTier::Medium;
        destinations_.push_back(std::move(slot));
        streams.push_back(MediaClientStream{
            destination.client_id,
            destination.destination_host,
            MediaEndpoint{rtp_h264_uri(destination.destination_host, destination.port), ""},
        });
    }
    if (!destinations_.empty()) {
        restart_pipeline();
    }
    return streams;
}

std::vector<MediaClientStream> GStreamerVideoFanout::start_pipewire(
    const std::string& pipewire_node,
    const std::vector<MediaStreamRequest>& destinations) {
    if (!destinations_.empty() || process_.running()) {
        throw std::runtime_error("video fanout is already running");
    }

    source_kind_ = SourceKind::PipeWire;
    display_.clear();
    pipewire_node_ = pipewire_node;
    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());
    for (const auto& destination : destinations) {
        Destination slot{};
        slot.client_id = destination.client_id;
        slot.host = destination.destination_host;
        slot.base_port = destination.port;
        slot.port = destination.port;
        slot.tier = MediaQualityTier::Medium;
        destinations_.push_back(std::move(slot));
        streams.push_back(MediaClientStream{
            destination.client_id,
            destination.destination_host,
            MediaEndpoint{rtp_h264_uri(destination.destination_host, destination.port), ""},
        });
    }
    if (!destinations_.empty()) {
        restart_pipeline();
    }
    return streams;
}

MediaClientStream GStreamerVideoFanout::add(
    const std::string& display,
    const MediaStreamRequest& destination,
    const VideoEncodeSettings& settings) {
    display_ = display;
    stop_client(destination.client_id);

    Destination slot{};
    slot.client_id = destination.client_id;
    slot.host = destination.destination_host;
    slot.base_port = destination.port;
    slot.port = destination.port;
    slot.tier = settings.bitrate_kbps == 0
        ? MediaQualityTier::Medium
        : media_quality_tier_for_settings(settings);
    destinations_.push_back(std::move(slot));
    restart_pipeline();

    return MediaClientStream{
        destination.client_id,
        destination.destination_host,
        MediaEndpoint{rtp_h264_uri(destination.destination_host, destination.port), ""},
    };
}

std::optional<std::string> GStreamerVideoFanout::begin_tier_cutover(
    ClientId client_id,
    const VideoEncodeSettings& settings) {
    Destination* slot = find_destination(client_id);
    if (slot == nullptr) {
        return std::nullopt;
    }
    if (source_kind_ == SourceKind::X11 && display_.empty()) {
        return std::nullopt;
    }
    if (source_kind_ == SourceKind::PipeWire && pipewire_node_.empty()) {
        return std::nullopt;
    }
    if (slot->staging_active) {
        return std::nullopt;
    }
    const auto tier = media_quality_tier_for_settings(settings);
    if (slot->tier == tier) {
        return std::nullopt;
    }

    const std::uint16_t staging_port =
        slot->port == slot->base_port
            ? static_cast<std::uint16_t>(slot->base_port + 16)
            : slot->base_port;

    const auto staging_log = staging_encode_log_path();
    try {
        terminate_gst_multiudpsink_on_port(staging_port);
        auto args = build_single_encode_args(tier, slot->host, staging_port);
        apply_nvenc_environment(slot->staging, std::move(args), staging_log);
        // ximagesrc + nvenc can take ~1s to fail (context/session errors surface
        // at first frame). Telling the client about a pipeline that is about to
        // die is what strands it on a silent port.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        if (!slot->staging.running()) {
            slot->staging.stop();
            std::cerr
                << "Video tier cutover staging exited immediately on port " << staging_port
                << "; see " << staging_log << '\n';
            return std::nullopt;
        }
    } catch (const std::exception& error) {
        std::cerr << "Video tier cutover staging failed: " << error.what() << '\n';
        slot->staging.stop();
        return std::nullopt;
    }

    slot->staging_active = true;
    slot->staging_port = staging_port;
    slot->staging_tier = tier;
    slot->staging_started = std::chrono::steady_clock::now();
    return rtp_h264_uri(slot->host, staging_port);
}

bool GStreamerVideoFanout::complete_tier_cutover(
    ClientId client_id,
    std::string_view staging_video_uri) {
    Destination* slot = find_destination(client_id);
    if (slot == nullptr || !slot->staging_active) {
        return false;
    }
    const auto expected = rtp_h264_uri(slot->host, slot->staging_port);
    if (staging_video_uri != expected) {
        return false;
    }
    if (!slot->staging.running()) {
        abort_tier_cutover(client_id);
        return false;
    }

    const bool was_on_shared_tee = !slot->dedicated.running();
    if (slot->dedicated.running()) {
        slot->dedicated.stop();
    }
    // Promote staging process to dedicated (client leaves shared tee).
    slot->dedicated = std::move(slot->staging);
    slot->staging = ChildProcess{};
    slot->port = slot->staging_port;
    slot->tier = slot->staging_tier;
    slot->staging_active = false;
    slot->staging_port = 0;

    if (was_on_shared_tee) {
        // Drop this client from the shared ladder (may briefly affect remaining tee clients).
        restart_pipeline();
    }

    std::cout << "Video cutover complete for client " << static_cast<int>(client_id)
              << " -> " << media_quality_tier_name(slot->tier)
              << " on port " << slot->port << '\n';
    return true;
}

void GStreamerVideoFanout::abort_tier_cutover(ClientId client_id) {
    Destination* slot = find_destination(client_id);
    if (slot == nullptr || !slot->staging_active) {
        return;
    }
    slot->staging.stop();
    if (slot->staging_port != 0) {
        terminate_gst_multiudpsink_on_port(slot->staging_port);
    }
    slot->staging_active = false;
    slot->staging_port = 0;
    std::cerr << "Video cutover aborted for client " << static_cast<int>(client_id) << '\n';
}

bool GStreamerVideoFanout::cutover_in_flight(ClientId client_id) const {
    const Destination* slot = find_destination(client_id);
    return slot != nullptr && slot->staging_active;
}

void GStreamerVideoFanout::stop() {
    for (auto& destination : destinations_) {
        if (destination.staging_active) {
            destination.staging.stop();
            destination.staging_active = false;
        }
        destination.dedicated.stop();
    }
    process_.stop();
    destinations_.clear();
    display_.clear();
}

void GStreamerVideoFanout::stop_client(ClientId client_id) {
    Destination* slot = find_destination(client_id);
    if (slot == nullptr) {
        return;
    }
    const bool was_on_shared_tee = !slot->dedicated.running();
    if (slot->staging_active) {
        abort_tier_cutover(client_id);
    }
    slot->dedicated.stop();
    destinations_.erase(
        std::remove_if(
            destinations_.begin(),
            destinations_.end(),
            [client_id](const Destination& destination) {
                return destination.client_id == client_id;
            }),
        destinations_.end());
    if (was_on_shared_tee) {
        if (destinations_.empty()) {
            process_.stop();
            return;
        }
        // Only rebuild shared tee when the removed client was on it.
        bool any_shared = false;
        for (const auto& destination : destinations_) {
            if (!destination.dedicated.running()) {
                any_shared = true;
                break;
            }
        }
        if (any_shared) {
            restart_pipeline();
        } else {
            process_.stop();
        }
    }
}

void GStreamerVideoFanout::restart_pipeline() {
    process_.stop();

    std::vector<std::pair<std::string, std::uint16_t>> very_high_clients;
    std::vector<std::pair<std::string, std::uint16_t>> high_clients;
    std::vector<std::pair<std::string, std::uint16_t>> medium_high_clients;
    std::vector<std::pair<std::string, std::uint16_t>> medium_clients;
    std::vector<std::pair<std::string, std::uint16_t>> low_clients;
    for (const auto& destination : destinations_) {
        if (destination.dedicated.running()) {
            continue;
        }
        const auto client = std::make_pair(destination.host, destination.port);
        switch (destination.tier) {
        case MediaQualityTier::Low:
            low_clients.push_back(client);
            break;
        case MediaQualityTier::Medium:
        case MediaQualityTier::Auto:
            medium_clients.push_back(client);
            break;
        case MediaQualityTier::MediumHigh:
            medium_high_clients.push_back(client);
            break;
        case MediaQualityTier::VeryHigh:
            very_high_clients.push_back(client);
            break;
        case MediaQualityTier::High:
        default:
            high_clients.push_back(client);
            break;
        }
    }

    const int active_tiers =
        (very_high_clients.empty() ? 0 : 1) +
        (high_clients.empty() ? 0 : 1) +
        (medium_high_clients.empty() ? 0 : 1) +
        (medium_clients.empty() ? 0 : 1) +
        (low_clients.empty() ? 0 : 1);
    if (active_tiers == 0) {
        return;
    }

    // Crash leftovers (e.g. black ximagesrc) can keep publishing on the same RTP ports.
    for (const auto& destination : destinations_) {
        if (!destination.dedicated.running()) {
            terminate_gst_multiudpsink_on_port(destination.port);
        }
    }
    if (source_kind_ == SourceKind::X11 && display_.empty()) {
        return;
    }
    if (source_kind_ == SourceKind::PipeWire && pipewire_node_.empty()) {
        return;
    }
    if (!gst_element_available("multiudpsink")) {
        throw std::runtime_error("multiudpsink is required for video ladder fanout (gst-plugins-good)");
    }
    if (source_kind_ == SourceKind::PipeWire && !gst_element_available("pipewiresrc")) {
        throw std::runtime_error("pipewiresrc is required for gamescope video capture (gst-plugin-pipewire)");
    }

    auto append_h264_branch = [&](
        std::vector<std::string>& args,
        const VideoEncodeSettings& settings,
        const std::vector<std::pair<std::string, std::uint16_t>>& clients,
        bool use_nvenc) {
        const int bitrate = settings.bitrate_kbps == 0 ? 1500 : settings.bitrate_kbps;
        const int framerate = settings.framerate == 0 ? 30 : static_cast<int>(settings.framerate);
        // Keep IDRs very frequent on Wi‑Fi: a lost scene-cut (credits→title) must
        // recover within a few hundred ms, not when continuous animation starts.
        const int configured_key_int =
            settings.key_int_max == 0 ? framerate : static_cast<int>(settings.key_int_max);
        const int sixth_sec = std::max(5, framerate / 6);
        const int key_int_max = std::min(configured_key_int, sixth_sec);

        // Live capture must drop OLD frames under backpressure, never NEW ones.
        // Keep the queue tiny: max-size-buffers=4 held up to ~4 frames (~130 ms at
        // 30 fps) before encode, which dominated felt button lag after pad UDP.
        args.insert(args.end(), {
            "queue",
            "max-size-buffers=1",
            "max-size-time=0",
            "max-size-bytes=0",
            "leaky=upstream",
            "!",
        });
        if (settings.width > 0 && settings.height > 0) {
            args.insert(args.end(), {
                "videoscale",
                "method=0",
                "!",
                "video/x-raw,width=" + std::to_string(settings.width) +
                    ",height=" + std::to_string(settings.height),
                "!",
            });
        }
        args.insert(args.end(), {
            "videorate",
            "drop-only=true",
            "!",
            "video/x-raw,framerate=" + std::to_string(framerate) + "/1",
            "!",
        });
        if (use_nvenc) {
            args.insert(args.end(), {
                "nvh264enc",
                "zerolatency=true",
                "preset=low-latency-hp",
                "strict-gop=true",
                "bitrate=" + std::to_string(bitrate),
                "gop-size=" + std::to_string(key_int_max),
                "!",
                "video/x-h264,profile=baseline,stream-format=byte-stream",
            });
        } else {
            args.insert(args.end(), {
                "x264enc",
                "tune=zerolatency",
                "speed-preset=ultrafast",
                "bitrate=" + std::to_string(bitrate),
                "key-int-max=" + std::to_string(key_int_max),
                "byte-stream=true",
                "bframes=0",
                "threads=1",
                "option-string=scenecut=40",
                "!",
                "video/x-h264,profile=constrained-baseline,stream-format=byte-stream",
            });
        }
        if (gst_element_available("h264parse")) {
            args.insert(args.end(), {"!", "h264parse", "config-interval=-1"});
        }
        args.insert(args.end(), {
            "!",
            "rtph264pay",
            "mtu=1200",
            "config-interval=-1",
            "aggregate-mode=zero-latency",
            "pt=96",
            "!",
            "multiudpsink",
            "clients=" + multiudp_clients_arg(clients),
            "sync=false",
            "async=false",
        });
    };

    const bool nvenc = gst_element_available("nvh264enc");
    auto args = std::vector<std::string>{"gst-launch-1.0", "-q"};
    if (source_kind_ == SourceKind::PipeWire) {
        args.insert(args.end(), {
            "pipewiresrc",
            "path=" + pipewire_node_,
            "do-timestamp=true",
            "!",
            "video/x-raw,format=BGRx",
            "!",
            "videoconvert",
        });
    } else {
        args.insert(args.end(), {
            "ximagesrc",
            "display-name=" + display_,
            // Poll continuously — XDamage often misses GL swaps on Xvfb for mostly-static
            // GB screens (credits/title), which froze remotes until animation started.
            "use-damage=false",
            "show-pointer=false",
            "do-timestamp=true",
            "!",
            "videoconvert",
        });
    }

    if (active_tiers == 1) {
        // Single tier: no tee needed.
        const auto* clients = !very_high_clients.empty() ? &very_high_clients
            : !high_clients.empty() ? &high_clients
            : !medium_high_clients.empty() ? &medium_high_clients
            : !medium_clients.empty() ? &medium_clients
            : &low_clients;
        const auto tier = !very_high_clients.empty() ? MediaQualityTier::VeryHigh
            : !high_clients.empty() ? MediaQualityTier::High
            : !medium_high_clients.empty() ? MediaQualityTier::MediumHigh
            : !medium_clients.empty() ? MediaQualityTier::Medium
            : MediaQualityTier::Low;
        const auto settings = video_encode_settings_for_tier(tier);
        args.push_back("!");
        // Prefer nvenc for any tier when available — switching encoders mid-session
        // forced full pipeline rebuilds and visible freezes.
        append_h264_branch(args, settings, *clients, nvenc);
    } else {
        args.insert(args.end(), {"!", "tee", "name=t"});
        auto append_tier = [&](
            const std::vector<std::pair<std::string, std::uint16_t>>& clients,
            MediaQualityTier tier) {
            if (clients.empty()) {
                return;
            }
            args.push_back("t.");
            args.push_back("!");
            append_h264_branch(
                args,
                video_encode_settings_for_tier(tier),
                clients,
                nvenc);
        };
        append_tier(very_high_clients, MediaQualityTier::VeryHigh);
        append_tier(high_clients, MediaQualityTier::High);
        append_tier(medium_high_clients, MediaQualityTier::MediumHigh);
        append_tier(medium_clients, MediaQualityTier::Medium);
        append_tier(low_clients, MediaQualityTier::Low);
    }

    // nvidia-smi / Host GPU indices use PCI order. CUDA defaults to
    // FASTEST_FIRST, so CUDA_VISIBLE_DEVICES=0 would pick the 3060 on a
    // 1660+3060 box unless PCI_BUS_ID order is forced.
    apply_nvenc_environment(process_, std::move(args));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!process_.running()) {
        throw std::runtime_error(
            source_kind_ == SourceKind::PipeWire
                ? "video capture pipeline exited immediately (need pipewiresrc, x264enc/nvh264enc, multiudpsink)"
                : "video capture pipeline exited immediately (need Xvfb/Xephyr, ximagesrc, x264enc, multiudpsink)");
    }

    std::cout << "Video ladder ("
              << (source_kind_ == SourceKind::PipeWire ? "pipewire" : "ximagesrc")
              << (nvenc ? ", nvenc" : ", x264");
    if (nvenc && nvenc_cuda_device_id_ >= 0) {
        std::cout << " cuda=" << nvenc_cuda_device_id_;
    }
    std::cout << "):";
    auto log_tier = [](const char* label, const std::vector<std::pair<std::string, std::uint16_t>>& clients, MediaQualityTier tier) {
        if (clients.empty()) {
            return;
        }
        const auto s = video_encode_settings_for_tier(tier);
        std::cout << " " << label << "=" << clients.size()
                  << "@" << s.bitrate_kbps << "kbps/" << static_cast<int>(s.framerate) << "fps";
        if (s.width > 0 && s.height > 0) {
            std::cout << "/" << s.width << "x" << s.height;
        }
    };
    log_tier("vhigh", very_high_clients, MediaQualityTier::VeryHigh);
    log_tier("high", high_clients, MediaQualityTier::High);
    log_tier("mhigh", medium_high_clients, MediaQualityTier::MediumHigh);
    log_tier("med", medium_clients, MediaQualityTier::Medium);
    log_tier("low", low_clients, MediaQualityTier::Low);
    std::cout << '\n';
}

GStreamerAudioFanout::~GStreamerAudioFanout() {
    stop();
}

std::vector<MediaClientStream> GStreamerAudioFanout::start(
    AudioCaptureBackend backend,
    const std::string& source,
    const std::vector<MediaStreamRequest>& destinations) {
    if (!destinations_.empty() || process_.running()) {
        throw std::runtime_error("audio fanout is already running");
    }

    backend_ = backend;
    source_ = source;
    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());
    for (const auto& destination : destinations) {
        destinations_.push_back(Destination{
            destination.client_id,
            destination.destination_host,
            destination.port,
        });
        streams.push_back(MediaClientStream{
            destination.client_id,
            destination.destination_host,
            MediaEndpoint{"", rtp_opus_uri(destination.destination_host, destination.port)},
        });
    }
    if (!destinations_.empty()) {
        restart_pipeline();
    }
    return streams;
}

MediaClientStream GStreamerAudioFanout::add(
    AudioCaptureBackend backend,
    const std::string& source,
    const MediaStreamRequest& destination) {
    backend_ = backend;
    source_ = source;
    stop_client(destination.client_id);

    destinations_.push_back(Destination{
        destination.client_id,
        destination.destination_host,
        destination.port,
    });
    restart_pipeline();

    return MediaClientStream{
        destination.client_id,
        destination.destination_host,
        MediaEndpoint{"", rtp_opus_uri(destination.destination_host, destination.port)},
    };
}

void GStreamerAudioFanout::stop() {
    process_.stop();
    destinations_.clear();
}

void GStreamerAudioFanout::stop_client(ClientId client_id) {
    const auto before = destinations_.size();
    destinations_.erase(
        std::remove_if(
            destinations_.begin(),
            destinations_.end(),
            [client_id](const Destination& destination) {
                return destination.client_id == client_id;
            }),
        destinations_.end());
    if (destinations_.size() == before) {
        return;
    }
    if (destinations_.empty()) {
        process_.stop();
        return;
    }
    restart_pipeline();
}

void GStreamerAudioFanout::restart() {
    restart_pipeline();
}

void GStreamerAudioFanout::restart_pipeline() {
    process_.stop();
    if (destinations_.empty()) {
        return;
    }
    for (const auto& destination : destinations_) {
        terminate_gst_multiudpsink_on_port(destination.port);
    }
    if (!gst_element_available("multiudpsink")) {
        throw std::runtime_error("multiudpsink is required for shared audio fanout (gst-plugins-good)");
    }

    std::vector<std::pair<std::string, std::uint16_t>> clients;
    clients.reserve(destinations_.size());
    for (const auto& destination : destinations_) {
        clients.emplace_back(destination.host, destination.port);
    }

    auto args = std::vector<std::string>{
        "gst-launch-1.0",
        "-q",
    };
    // Pulse-style "*.monitor" names (e.g. archstreamer.monitor) must use pulsesrc.
    // pipewiresrc used to fall through to @DEFAULT_MONITOR@ whenever the name
    // contained ".monitor", which captured the wrong sink (HDMI/etc.) while
    // RetroArch played into the silent null sink — late/missing client audio and
    // audio_sync pacing stalls on the host (Space=FF looked like a "video unstick").
    const bool pulse_monitor =
        !source_.empty() && source_.size() > 8 &&
        source_.compare(source_.size() - 8, 8, ".monitor") == 0;
    if (backend_ == AudioCaptureBackend::Pulse || pulse_monitor) {
        args.push_back("pulsesrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        // Keep capture latency tight so the null-sink monitor stays awake and
        // RetroArch's audio_sync clock does not build a multi-second backlog.
        args.push_back("buffer-time=80000");
        args.push_back("latency-time=20000");
        args.push_back("provide-clock=false");
        if (!source_.empty()) {
            args.push_back("device=" + source_);
        }
    } else {
        args.push_back("pipewiresrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        if (!source_.empty()) {
            args.push_back("target-object=" + source_);
        } else {
            args.push_back("target-object=@DEFAULT_MONITOR@");
        }
    }
    args.insert(args.end(), {
        "!",
        "audioconvert",
        "!",
        "audioresample",
        "!",
        "audio/x-raw,rate=48000,channels=2",
        "!",
        "opusenc",
        "bitrate=128000",
        "frame-size=20",
        "inband-fec=true",
        "!",
        "rtpopuspay",
        "pt=97",
        "!",
        "multiudpsink",
        "clients=" + multiudp_clients_arg(clients),
        "sync=false",
        "async=false",
    });
    process_.start(std::move(args));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!process_.running()) {
        throw std::runtime_error(
            "audio capture pipeline exited immediately (need pulsesrc/pipewiresrc, opusenc, multiudpsink)");
    }
    std::cout
        << "Audio capture (shared): " << destinations_.size()
        << " destination(s) from "
        << (source_.empty() ? std::string("<default>") : source_)
        << '\n';
}

GStreamerMediaServer::GStreamerMediaServer(GStreamerMediaCaptureConfig capture)
    : capture_(std::move(capture)) {
}

void GStreamerMediaServer::start(
    const HostMediaPlanConfig& plan,
    const std::vector<HostMediaDestination>& destinations,
    std::vector<MediaClientStream>& streams) {
    plan_ = plan;
    destinations_ = destinations;
    if (capture_.video) {
        virtual_display_ = make_virtual_display(capture_.display_backend);
        if (virtual_display_) {
            virtual_display_->start(capture_.virtual_display, capture_.video_resolution);
            if (capture_.verbose) {
                std::cout << "Virtual display backend=";
                switch (virtual_display_->backend()) {
                case VirtualDisplayBackend::Gamescope:
                    std::cout << "gamescope (headless + PipeWire)";
                    break;
                case VirtualDisplayBackend::VirtualGL:
                    std::cout << "virtualgl (Xvfb + vglrun)";
                    break;
                case VirtualDisplayBackend::Xephyr:
                    std::cout << "xephyr on " << capture_.virtual_display;
                    break;
                case VirtualDisplayBackend::Xvfb:
                    std::cout << "xvfb on " << capture_.virtual_display;
                    break;
                case VirtualDisplayBackend::None:
                    std::cout << "none";
                    break;
                }
                std::cout << '\n';
            }
        }

        defer_pipewire_video_ =
            virtual_display_ && virtual_display_->uses_pipewire_video();
        if (defer_pipewire_video_) {
            // Assign RTP URIs now; attach pipewiresrc after gamescope publishes its node.
            const auto requests = video_requests_from_media_destinations(plan, destinations);
            for (const auto& request : requests) {
                for (auto& media_stream : streams) {
                    if (media_stream.client_id == request.client_id) {
                        media_stream.endpoint.video_uri =
                            rtp_h264_uri(request.destination_host, request.port);
                    }
                }
            }
            if (capture_.verbose) {
                std::cout << "Video capture deferred until gamescope PipeWire node is ready.\n";
            }
        } else {
            video_fanout_.emplace();
            video_fanout_->set_nvenc_cuda_device_id(capture_.nvenc_cuda_device_id);
            const auto video_streams = video_fanout_->start(
                capture_.virtual_display,
                video_requests_from_media_destinations(plan, destinations));
            for (const auto& stream : video_streams) {
                for (auto& media_stream : streams) {
                    if (media_stream.client_id == stream.client_id) {
                        media_stream.endpoint.video_uri = stream.endpoint.video_uri;
                    }
                }
            }
        }
    }
    if (capture_.audio) {
        try {
            audio_fanout_.emplace();
            const auto audio_streams = audio_fanout_->start(
                capture_.audio_backend,
                capture_.audio_source,
                audio_requests_from_media_destinations(plan, destinations));
            for (const auto& stream : audio_streams) {
                for (auto& media_stream : streams) {
                    if (media_stream.client_id == stream.client_id) {
                        media_stream.endpoint.audio_uri = stream.endpoint.audio_uri;
                    }
                }
            }
        } catch (const std::exception& error) {
            audio_fanout_.reset();
            capture_.audio = false;
            std::cerr << "Warning: audio streaming disabled: " << error.what() << '\n';
        }
    }
}

bool GStreamerMediaServer::video_deferred() const {
    return defer_pipewire_video_ && !video_fanout_.has_value();
}

void GStreamerMediaServer::start_pipewire_video(
    const std::string& pipewire_node,
    std::vector<MediaClientStream>& streams) {
    if (!capture_.video || pipewire_node.empty()) {
        return;
    }
    if (video_fanout_.has_value()) {
        video_fanout_->stop();
        video_fanout_.reset();
    }
    video_fanout_.emplace();
    video_fanout_->set_nvenc_cuda_device_id(capture_.nvenc_cuda_device_id);
    const auto video_streams = video_fanout_->start_pipewire(
        pipewire_node,
        video_requests_from_media_destinations(plan_, destinations_));
    for (const auto& stream : video_streams) {
        for (auto& media_stream : streams) {
            if (media_stream.client_id == stream.client_id) {
                media_stream.endpoint.video_uri = stream.endpoint.video_uri;
            }
        }
    }
    defer_pipewire_video_ = false;
}

MediaEndpoint GStreamerMediaServer::add_client(
    ClientId client_id,
    const std::string& destination_host,
    std::size_t media_index,
    bool wants_video,
    bool wants_audio) {
    auto endpoint = MediaEndpoint{};
    const auto destination = HostMediaDestination{client_id, destination_host};
    if (wants_video && capture_.video && video_fanout_.has_value()) {
        const auto stream = video_fanout_->add(
            capture_.virtual_display,
            video_request_for_destination(plan_, destination, media_index));
        endpoint.video_uri = stream.endpoint.video_uri;
    }
    if (wants_audio && capture_.audio && audio_fanout_.has_value()) {
        const auto stream = audio_fanout_->add(
            capture_.audio_backend,
            capture_.audio_source,
            audio_request_for_destination(plan_, destination, media_index));
        endpoint.audio_uri = stream.endpoint.audio_uri;
    }
    return endpoint;
}

void GStreamerMediaServer::remove_client(ClientId client_id) {
    if (video_fanout_.has_value()) {
        video_fanout_->stop_client(client_id);
    }
    if (audio_fanout_.has_value()) {
        audio_fanout_->stop_client(client_id);
    }
}

bool GStreamerMediaServer::complete_video_tier_cutover(
    ClientId client_id,
    std::string_view staging_video_uri) {
    if (!video_fanout_.has_value()) {
        return false;
    }
    return video_fanout_->complete_tier_cutover(client_id, staging_video_uri);
}

void GStreamerMediaServer::abort_video_tier_cutover(ClientId client_id) {
    if (video_fanout_.has_value()) {
        video_fanout_->abort_tier_cutover(client_id);
    }
}

bool GStreamerMediaServer::video_cutover_in_flight(ClientId client_id) const {
    return video_fanout_.has_value() && video_fanout_->cutover_in_flight(client_id);
}

std::optional<std::string> GStreamerMediaServer::begin_video_tier_cutover(
    ClientId client_id,
    const VideoEncodeSettings& settings) {
    if (!video_fanout_.has_value()) {
        return std::nullopt;
    }
    return video_fanout_->begin_tier_cutover(client_id, settings);
}

void GStreamerMediaServer::stop() {
    if (audio_fanout_.has_value()) {
        audio_fanout_->stop();
        audio_fanout_.reset();
    }
    if (video_fanout_.has_value()) {
        video_fanout_->stop();
        video_fanout_.reset();
    }
    if (virtual_display_) {
        virtual_display_->stop();
        virtual_display_.reset();
    }
}

std::unique_ptr<MediaServer> make_gstreamer_media_server(const GStreamerMediaCaptureConfig& capture) {
    return std::make_unique<GStreamerMediaServer>(capture);
}

} // namespace archstreamer
