#include "host/streaming_audio_sink.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace archstreamer {
namespace {

std::mutex g_track_mutex;
// slot_index -> root emulator PID (-1 key = single-session)
std::unordered_map<int, DWORD> g_tracked_roots;
std::set<DWORD> g_muted_pids;

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool looks_like_emulator_image(const std::string& image_lower) {
    static const char* kNames[] = {
        "retroarch.exe",
        "ryujinx.exe",
        "yuzu.exe",
        "suyu.exe",
        "sudachi.exe",
        "pcsx2.exe",
        "dolphin.exe",
        "ppsspp.exe",
        "citra.exe",
        "lime3ds.exe",
        "mupen64plus.exe",
        "mgba.exe",
    };
    for (const char* name : kNames) {
        if (image_lower.find(name) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string process_image_name(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    wchar_t path_w[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::string result;
    if (QueryFullProcessImageNameW(process, 0, path_w, &size)) {
        const std::filesystem::path path(path_w);
        result = path.filename().string();
    }
    CloseHandle(process);
    return result;
}

void collect_process_tree(DWORD root, std::set<DWORD>& out) {
    if (root == 0 || !out.insert(root).second) {
        return;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> children;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ParentProcessID == root) {
                children.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    for (DWORD child : children) {
        collect_process_tree(child, out);
    }
}

std::set<DWORD> target_pids_for_park(int slot_index) {
    std::set<DWORD> targets;
    {
        std::lock_guard lock(g_track_mutex);
        if (slot_index >= 0) {
            if (const auto it = g_tracked_roots.find(slot_index); it != g_tracked_roots.end()) {
                collect_process_tree(it->second, targets);
            }
        } else {
            if (const auto it = g_tracked_roots.find(-1); it != g_tracked_roots.end()) {
                collect_process_tree(it->second, targets);
            }
            // Single-session / unscoped park: also mute any known emulator by name.
            // (Tracked tree is preferred when present.)
        }
    }
    return targets;
}

struct ComInit {
    ComInit() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        need_uninit = SUCCEEDED(hr);
    }
    ~ComInit() {
        if (need_uninit) {
            CoUninitialize();
        }
    }
    bool ok = false;
    bool need_uninit = false;
};

int set_mute_for_matching_sessions(const std::set<DWORD>& prefer_pids, bool mute, bool by_name) {
    ComInit com;
    if (!com.ok) {
        return 0;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
        return 0;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || device == nullptr) {
        enumerator->Release();
        return 0;
    }

    IAudioSessionManager2* manager = nullptr;
    hr = device->Activate(
        __uuidof(IAudioSessionManager2),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(&manager));
    device->Release();
    enumerator->Release();
    if (FAILED(hr) || manager == nullptr) {
        return 0;
    }

    IAudioSessionEnumerator* sessions = nullptr;
    hr = manager->GetSessionEnumerator(&sessions);
    if (FAILED(hr) || sessions == nullptr) {
        manager->Release();
        return 0;
    }

    int count = 0;
    sessions->GetCount(&count);
    int changed = 0;
    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        if (FAILED(sessions->GetSession(i, &control)) || control == nullptr) {
            continue;
        }
        IAudioSessionControl2* control2 = nullptr;
        if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) ||
            control2 == nullptr) {
            control->Release();
            continue;
        }
        control->Release();

        DWORD pid = 0;
        if (FAILED(control2->GetProcessId(&pid)) || pid == 0) {
            control2->Release();
            continue;
        }

        bool match = !prefer_pids.empty() && prefer_pids.count(pid) != 0;
        if (!match && by_name) {
            match = looks_like_emulator_image(to_lower(process_image_name(pid)));
        }
        if (!match) {
            control2->Release();
            continue;
        }

        ISimpleAudioVolume* volume = nullptr;
        if (SUCCEEDED(control2->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume))) &&
            volume != nullptr) {
            BOOL currently = FALSE;
            volume->GetMute(&currently);
            if (static_cast<bool>(currently) != mute) {
                if (SUCCEEDED(volume->SetMute(mute ? TRUE : FALSE, nullptr))) {
                    ++changed;
                    std::lock_guard lock(g_track_mutex);
                    if (mute) {
                        g_muted_pids.insert(pid);
                    } else {
                        g_muted_pids.erase(pid);
                    }
                }
            } else if (mute) {
                std::lock_guard lock(g_track_mutex);
                g_muted_pids.insert(pid);
            }
            volume->Release();
        }
        control2->Release();
    }

    sessions->Release();
    manager->Release();
    return changed;
}

} // namespace

std::string StreamingAudioSink::slot_sink_name(int slot_index) {
    if (slot_index < 0) {
        slot_index = 0;
    }
    return std::string(kName) + "-" + std::to_string(slot_index);
}

std::string StreamingAudioSink::slot_application_id(int slot_index) {
    if (slot_index < 0) {
        slot_index = 0;
    }
    return std::string(kName) + "-slot-" + std::to_string(slot_index);
}

bool StreamingAudioSink::is_streaming_sink_name(std::string_view sink_name) {
    return sink_name == kName || sink_name.rfind("archstreamer-", 0) == 0;
}

std::string StreamingAudioSink::ensure() { return kName; }
std::string StreamingAudioSink::monitor_source() { return std::string(kName) + ".monitor"; }
std::string StreamingAudioSink::ensure_slot(int slot_index) { return slot_sink_name(slot_index); }
std::string StreamingAudioSink::monitor_source_for_slot(int slot_index) {
    return slot_sink_name(slot_index) + ".monitor";
}

void StreamingAudioSink::prune_unused(int /*max_slots*/, bool /*keep_legacy*/) {}

void StreamingAudioSink::track_emulator_process(int process_id, int slot_index) {
    if (process_id <= 0) {
        return;
    }
    std::lock_guard lock(g_track_mutex);
    g_tracked_roots[slot_index] = static_cast<DWORD>(process_id);
}

void StreamingAudioSink::untrack_emulator_process(int slot_index) {
    std::lock_guard lock(g_track_mutex);
    g_tracked_roots.erase(slot_index);
}

void StreamingAudioSink::park_game_audio() {
    const auto prefer = target_pids_for_park(-1);
    const int muted = set_mute_for_matching_sessions(prefer, /*mute=*/true, /*by_name=*/true);
    if (muted > 0) {
        std::cout
            << "Parked " << muted
            << " emulator audio session(s) (WASAPI mute; speakers stay quiet "
               "unless Watch stream locally).\n";
    }
}

void StreamingAudioSink::park_game_audio_for_slot(int slot_index) {
    const auto prefer = target_pids_for_park(slot_index);
    // Prefer PID tree for the slot; fall back to name match so first park before
    // track_emulator_process still silences something useful.
    const int muted = set_mute_for_matching_sessions(
        prefer,
        /*mute=*/true,
        /*by_name=*/prefer.empty());
    if (muted > 0) {
        std::cout
            << "Parked " << muted
            << " audio session(s) for slot " << slot_index << " (WASAPI mute).\n";
    }
}

void StreamingAudioSink::restore_default_sink() {
    std::set<DWORD> muted;
    {
        std::lock_guard lock(g_track_mutex);
        muted = g_muted_pids;
    }
    if (muted.empty()) {
        // Also unmute any known emulator sessions that may have been left muted.
        set_mute_for_matching_sessions({}, /*mute=*/false, /*by_name=*/true);
        return;
    }
    const int restored = set_mute_for_matching_sessions(muted, /*mute=*/false, /*by_name=*/false);
    if (restored > 0) {
        std::cout << "Restored " << restored << " muted emulator audio session(s).\n";
    }
    std::lock_guard lock(g_track_mutex);
    g_muted_pids.clear();
}

std::string StreamingAudioSink::default_monitor_source() {
    // WindowsMediaServer uses wasapisrc loopback on the default render device;
    // there is no Pulse-style ".monitor" name to return.
    return {};
}

} // namespace archstreamer

#endif // _WIN32
