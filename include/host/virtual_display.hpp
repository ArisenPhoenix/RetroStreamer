#pragma once

#include "common/platform/default_platform.hpp"
#include "host/media_capture.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

// Owned virtual X server used for isolated capture (ximagesrc), or a Gamescope
// session that publishes frames on PipeWire instead.
//
// VirtualGlDisplay: Xvfb + vglrun (OpenGL). Kept for later revisit.
// GamescopeDisplay: headless gamescope wrapper; capture via PipeWire node.
class VirtualDisplay {
public:
    virtual ~VirtualDisplay();

    virtual void start(const std::string& display, const std::string& resolution) = 0;
    virtual void stop() = 0;

    [[nodiscard]] virtual std::string display() const = 0;
    [[nodiscard]] virtual VirtualDisplayBackend backend() const = 0;

    // Argv prefix for GPU-accelerated clients (empty for plain Xvfb/Xephyr).
    [[nodiscard]] virtual std::vector<std::string> gl_command_prefix() const;
    [[nodiscard]] virtual std::vector<std::pair<std::string, std::string>> gl_environment() const;
    [[nodiscard]] virtual bool accelerates_opengl() const;
    // When true, video is captured from PipeWire (gamescope) rather than ximagesrc.
    [[nodiscard]] virtual bool uses_pipewire_video() const;
};

class XvfbDisplay : public VirtualDisplay {
public:
    ~XvfbDisplay() override;

    void start(const std::string& display, const std::string& resolution) override;
    void stop() override;

    [[nodiscard]] std::string display() const override;
    [[nodiscard]] VirtualDisplayBackend backend() const override;

protected:
    std::string display_;
    ChildProcess process_;
};

class XephyrDisplay : public VirtualDisplay {
public:
    ~XephyrDisplay() override;

    void start(const std::string& display, const std::string& resolution) override;
    void stop() override;

    [[nodiscard]] std::string display() const override;
    [[nodiscard]] VirtualDisplayBackend backend() const override;

private:
    std::string display_;
    ChildProcess process_;
};

// Xvfb capture display + VirtualGL (vglrun) so OpenGL hits the real GPU.
class VirtualGlDisplay final : public XvfbDisplay {
public:
    [[nodiscard]] VirtualDisplayBackend backend() const override;
    [[nodiscard]] std::vector<std::string> gl_command_prefix() const override;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> gl_environment() const override;
    [[nodiscard]] bool accelerates_opengl() const override;
};

// Headless gamescope: no Xvfb. Frames are published as a PipeWire Video/Source
// (media.name=gamescope). Launch the app with gl_command_prefix().
class GamescopeDisplay final : public VirtualDisplay {
public:
    void start(const std::string& display, const std::string& resolution) override;
    void stop() override;

    [[nodiscard]] std::string display() const override;
    [[nodiscard]] VirtualDisplayBackend backend() const override;
    [[nodiscard]] std::vector<std::string> gl_command_prefix() const override;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> gl_environment() const override;
    [[nodiscard]] bool uses_pipewire_video() const override;

    void set_prefer_vk_device(std::string vendor_device);
    void set_nested_size(int width, int height);

private:
    std::string display_;
    std::string prefer_vk_device_;
    int width_ = 1280;
    int height_ = 720;
};

bool command_available(const char* command);
VirtualDisplayBackend choose_virtual_display_backend(VirtualDisplayBackend requested);
std::unique_ptr<VirtualDisplay> make_virtual_display(VirtualDisplayBackend backend);

[[nodiscard]] std::optional<std::string> find_vglrun();
[[nodiscard]] std::string default_vgl_display();
[[nodiscard]] std::vector<std::string> virtual_gl_command_prefix();
[[nodiscard]] std::vector<std::pair<std::string, std::string>> virtual_gl_environment();

[[nodiscard]] std::optional<std::string> find_gamescope();
[[nodiscard]] std::vector<std::string> gamescope_command_prefix(
    int width,
    int height,
    const std::string& prefer_vk_device);
[[nodiscard]] std::vector<std::pair<std::string, std::string>> gamescope_launch_environment();

/**
 * Nested XTest DISPLAY for gamescope session slot N (`:20+N`).
 * Pin only (ARCHSTREAMER_XTEST_DISPLAY); match sessions via ARCHSTREAMER_SESSION_ID
 * and the host lease map. The gamescope wrapper reserves lower numbers with
 * abstract bind-without-listen (per firejail netns) plus X lock files so nested
 * Xwayland lands here. Do not listen() on those sockets — that hangs gamescope.
 */
[[nodiscard]] std::string gamescope_xtest_display_for_slot(std::size_t slot_index);
inline constexpr const char* kArchstreamerXTestDisplayEnv = "ARCHSTREAMER_XTEST_DISPLAY";

// Map a sysfs-style PCI bus (0000:03:00.0 / 00000000:03:00.0) to "vendor:device" for
// gamescope --prefer-vk-device (e.g. 10de:2504).
[[nodiscard]] std::optional<std::string> pci_vendor_device_id(const std::string& pci_bus);

} // namespace archstreamer
