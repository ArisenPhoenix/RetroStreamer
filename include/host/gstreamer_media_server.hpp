#pragma once

#include "common/platform/default_platform.hpp"
#include "common/protocol.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace archstreamer {

enum class VirtualDisplayBackend {
    None,
    Xvfb,
    Xephyr,
};

enum class AudioCaptureBackend {
    Pulse,
    PipeWire,
};

inline bool command_available(const char* command) {
    const auto check_path = [command](const std::filesystem::path& directory) {
        if (directory.empty()) {
            return false;
        }
        const auto path = directory / command;
        return access(path.c_str(), X_OK) == 0;
    };

    const char* path_env = std::getenv("PATH");
    if (path_env != nullptr) {
        auto paths = std::string{path_env};
        std::string::size_type start = 0;
        while (start <= paths.size()) {
            const auto end = paths.find(':', start);
            const auto directory = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (check_path(directory)) {
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    return check_path("/usr/bin") || check_path("/usr/local/bin");
}

inline VirtualDisplayBackend choose_virtual_display_backend(VirtualDisplayBackend requested) {
    if (requested != VirtualDisplayBackend::None) {
        return requested;
    }
    if (command_available("Xvfb")) {
        return VirtualDisplayBackend::Xvfb;
    }
    if (command_available("Xephyr")) {
        return VirtualDisplayBackend::Xephyr;
    }

    throw std::runtime_error("no virtual display backend found; install Xvfb or Xephyr");
}

class VirtualDisplayProcess {
public:
    void start(VirtualDisplayBackend backend, const std::string& display, const std::string& resolution) {
        backend_ = choose_virtual_display_backend(backend);
        if (backend_ == VirtualDisplayBackend::Xvfb) {
            process_.start({"Xvfb", display, "-screen", "0", resolution + "x24", "-nolisten", "tcp"});
        } else if (backend_ == VirtualDisplayBackend::Xephyr) {
            process_.start({"Xephyr", display, "-screen", resolution, "-ac", "-noreset"});
        } else {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }

    void stop() {
        process_.stop();
    }

private:
    VirtualDisplayBackend backend_ = VirtualDisplayBackend::None;
    ChildProcess process_;
};

class GStreamerVideoSender {
public:
    MediaEndpoint endpoint(std::string host, std::uint16_t port) const {
        return MediaEndpoint{
            "rtp+h264://" + std::move(host) + ":" + std::to_string(port),
            "",
        };
    }

    void start(const std::string& display, const std::string& destination_host, std::uint16_t port) {
        process_.start({
            "gst-launch-1.0",
            "-q",
            "ximagesrc",
            "display-name=" + display,
            "use-damage=0",
            "show-pointer=false",
            "!",
            "video/x-raw,framerate=30/1",
            "!",
            "videoconvert",
            "!",
            "queue",
            "!",
            "x264enc",
            "tune=zerolatency",
            "speed-preset=ultrafast",
            "bitrate=2500",
            "key-int-max=30",
            "!",
            "rtph264pay",
            "config-interval=1",
            "pt=96",
            "!",
            "udpsink",
            "host=" + destination_host,
            "port=" + std::to_string(port),
            "sync=false",
            "async=false",
        });
    }

    void stop() {
        process_.stop();
    }

private:
    ChildProcess process_;
};

struct GStreamerRtpStreamRequest {
    ClientId client_id = 0;
    std::string destination_host;
    std::uint16_t port = 0;
};

struct GStreamerMediaStream {
    ClientId client_id = 0;
    std::string destination_host;
    MediaEndpoint endpoint;
};

class GStreamerVideoFanout {
public:
    std::vector<GStreamerMediaStream> start(
        const std::string& display,
        const std::vector<GStreamerRtpStreamRequest>& destinations) {
        if (!senders_.empty()) {
            throw std::runtime_error("video fanout is already running");
        }

        auto streams = std::vector<GStreamerMediaStream>{};
        streams.reserve(destinations.size());

        for (const auto& destination : destinations) {
            streams.push_back(add(display, destination));
        }

        return streams;
    }

    GStreamerMediaStream add(
        const std::string& display,
        const GStreamerRtpStreamRequest& destination) {
        auto sender = GStreamerVideoSender{};
        const auto endpoint = sender.endpoint(destination.destination_host, destination.port);
        sender.start(display, destination.destination_host, destination.port);
        stop_client(destination.client_id);
        senders_.emplace(destination.client_id, std::move(sender));
        return GStreamerMediaStream{
            destination.client_id,
            destination.destination_host,
            endpoint,
        };
    }

    void stop() {
        for (auto& [client_id, sender] : senders_) {
            (void)client_id;
            sender.stop();
        }
        senders_.clear();
    }

    void stop_client(ClientId client_id) {
        const auto sender = senders_.find(client_id);
        if (sender == senders_.end()) {
            return;
        }
        sender->second.stop();
        senders_.erase(sender);
    }

private:
    std::map<ClientId, GStreamerVideoSender> senders_;
};

class GStreamerAudioSender {
public:
    MediaEndpoint endpoint(std::string host, std::uint16_t port) const {
        return MediaEndpoint{
            "",
            "rtp+opus://" + std::move(host) + ":" + std::to_string(port),
        };
    }

    void start(
        AudioCaptureBackend backend,
        const std::string& source,
        const std::string& destination_host,
        std::uint16_t port) {
        auto args = std::vector<std::string>{
            "gst-launch-1.0",
            "-q",
        };

        if (backend == AudioCaptureBackend::Pulse) {
            args.push_back("pulsesrc");
            args.push_back("client-name=ArchStreamer");
            args.push_back("do-timestamp=true");
            if (!source.empty()) {
                args.push_back("device=" + source);
            }
        } else {
            args.push_back("pipewiresrc");
            args.push_back("client-name=ArchStreamer");
            if (!source.empty()) {
                args.push_back("target-object=" + source);
            }
        }

        const auto tail = std::vector<std::string>{
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
            "udpsink",
            "host=" + destination_host,
            "port=" + std::to_string(port),
            "sync=false",
            "async=false",
        };
        args.insert(args.end(), tail.begin(), tail.end());
        process_.start(std::move(args));
    }

    void stop() {
        process_.stop();
    }

private:
    ChildProcess process_;
};

class GStreamerAudioFanout {
public:
    std::vector<GStreamerMediaStream> start(
        AudioCaptureBackend backend,
        const std::string& source,
        const std::vector<GStreamerRtpStreamRequest>& destinations) {
        if (!senders_.empty()) {
            throw std::runtime_error("audio fanout is already running");
        }

        auto streams = std::vector<GStreamerMediaStream>{};
        streams.reserve(destinations.size());

        for (const auto& destination : destinations) {
            streams.push_back(add(backend, source, destination));
        }

        return streams;
    }

    GStreamerMediaStream add(
        AudioCaptureBackend backend,
        const std::string& source,
        const GStreamerRtpStreamRequest& destination) {
        auto sender = GStreamerAudioSender{};
        const auto endpoint = sender.endpoint(destination.destination_host, destination.port);
        sender.start(backend, source, destination.destination_host, destination.port);
        stop_client(destination.client_id);
        senders_.emplace(destination.client_id, std::move(sender));
        return GStreamerMediaStream{
            destination.client_id,
            destination.destination_host,
            endpoint,
        };
    }

    void stop() {
        for (auto& [client_id, sender] : senders_) {
            (void)client_id;
            sender.stop();
        }
        senders_.clear();
    }

    void stop_client(ClientId client_id) {
        const auto sender = senders_.find(client_id);
        if (sender == senders_.end()) {
            return;
        }
        sender->second.stop();
        senders_.erase(sender);
    }

private:
    std::map<ClientId, GStreamerAudioSender> senders_;
};

} // namespace archstreamer
