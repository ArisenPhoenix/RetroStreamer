#include "common/platform/windows_process.hpp"

#ifdef _WIN32

#include <stdexcept>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

std::string quote_windows_arg(const std::string& arg) {
    if (arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }

    std::string quoted = "\"";
    for (char ch : arg) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += "\"";
    return quoted;
}

std::string join_command_line(const std::vector<std::string>& args) {
    std::string command;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            command.push_back(' ');
        }
        command += quote_windows_arg(args[i]);
    }
    return command;
}

// CreateProcess will not append .exe when the token already contains a dot
// (e.g. gst-launch-1.0). Resolve via SearchPath so PATH lookups work.
std::string resolve_executable(const std::string& program) {
    if (program.find('\\') != std::string::npos || program.find('/') != std::string::npos) {
        return program;
    }

    char found[MAX_PATH] = {};
    const DWORD length = SearchPathA(nullptr, program.c_str(), ".exe", MAX_PATH, found, nullptr);
    if (length > 0 && length < MAX_PATH) {
        return std::string(found, length);
    }
    return program;
}

std::string build_environment_block(
    const std::vector<std::pair<std::string, std::string>>& env,
    const std::vector<std::string>& unset_env) {
    if (env.empty() && unset_env.empty()) {
        return {};
    }

    std::vector<std::pair<std::string, std::string>> merged;
    const char* inherited = GetEnvironmentStringsA();
    if (inherited != nullptr) {
        for (const char* cursor = inherited; *cursor != '\0';) {
            const std::string entry(cursor);
            cursor += entry.size() + 1;
            const auto eq = entry.find('=');
            if (eq == std::string::npos || eq == 0) {
                continue;
            }
            const auto key = entry.substr(0, eq);
            bool unset = false;
            for (const auto& name : unset_env) {
                if (name == key) {
                    unset = true;
                    break;
                }
            }
            if (!unset) {
                merged.emplace_back(key, entry.substr(eq + 1));
            }
        }
        FreeEnvironmentStringsA(const_cast<char*>(inherited));
    }

    for (const auto& [key, value] : env) {
        bool replaced = false;
        for (auto& existing : merged) {
            if (existing.first == key) {
                existing.second = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            merged.emplace_back(key, value);
        }
    }

    std::string block;
    for (const auto& [key, value] : merged) {
        block += key;
        block.push_back('=');
        block += value;
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
}

HANDLE open_nul_handle(DWORD access) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    return CreateFileA(
        "NUL",
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

} // namespace

WindowsChildProcess::~WindowsChildProcess() {
    stop();
}

WindowsChildProcess::WindowsChildProcess(WindowsChildProcess&& other) noexcept
    : process_info_(other.process_info_), started_(other.started_) {
    other.process_info_ = {};
    other.started_ = false;
}

WindowsChildProcess& WindowsChildProcess::operator=(WindowsChildProcess&& other) noexcept {
    if (this != &other) {
        stop();
        process_info_ = other.process_info_;
        started_ = other.started_;
        other.process_info_ = {};
        other.started_ = false;
    }
    return *this;
}

void WindowsChildProcess::start(
    const std::vector<std::string>& args,
    const std::vector<std::pair<std::string, std::string>>& env,
    const std::vector<std::string>& unset_env,
    const std::string& stderr_path) {
    if (args.empty()) {
        throw std::runtime_error("WindowsChildProcess::start requires at least one argument");
    }
    if (started_) {
        stop();
    }

    auto resolved_args = args;
    resolved_args[0] = resolve_executable(args[0]);
    auto command = join_command_line(resolved_args);
    auto env_block = build_environment_block(env, unset_env);

    // Never inherit the parent's full handle table (Qt/console timers, HWNDS, etc.).
    // That path exhausts USER objects and breaks d3d11videosink under the GUI.
    HANDLE stdin_handle = open_nul_handle(GENERIC_READ);
    HANDLE stdout_handle = open_nul_handle(GENERIC_WRITE);
    HANDLE stderr_handle = stdout_handle;
    HANDLE stderr_file = INVALID_HANDLE_VALUE;
    if (stdin_handle == INVALID_HANDLE_VALUE || stdout_handle == INVALID_HANDLE_VALUE) {
        if (stdin_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(stdin_handle);
        }
        if (stdout_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(stdout_handle);
        }
        throw std::runtime_error("failed to open NUL for child stdio");
    }

    if (!stderr_path.empty()) {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        stderr_file = CreateFileA(
            stderr_path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            &security,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (stderr_file == INVALID_HANDLE_VALUE) {
            CloseHandle(stdin_handle);
            CloseHandle(stdout_handle);
            throw std::runtime_error("failed to open child stderr path");
        }
        stderr_handle = stderr_file;
    }

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    auto* attr_bytes = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), 0, attr_size));
    if (attr_bytes == nullptr) {
        CloseHandle(stdin_handle);
        CloseHandle(stdout_handle);
        if (stderr_file != INVALID_HANDLE_VALUE) {
            CloseHandle(stderr_file);
        }
        throw std::runtime_error("failed to allocate process attribute list");
    }
    auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_bytes);
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
        HeapFree(GetProcessHeap(), 0, attr_bytes);
        CloseHandle(stdin_handle);
        CloseHandle(stdout_handle);
        if (stderr_file != INVALID_HANDLE_VALUE) {
            CloseHandle(stderr_file);
        }
        throw std::runtime_error("InitializeProcThreadAttributeList failed");
    }

    HANDLE inherit_handles[3];
    DWORD inherit_count = 0;
    inherit_handles[inherit_count++] = stdin_handle;
    inherit_handles[inherit_count++] = stdout_handle;
    if (stderr_handle != stdout_handle) {
        inherit_handles[inherit_count++] = stderr_handle;
    }

    if (!UpdateProcThreadAttribute(
            attr_list,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherit_handles,
            inherit_count * sizeof(HANDLE),
            nullptr,
            nullptr)) {
        DeleteProcThreadAttributeList(attr_list);
        HeapFree(GetProcessHeap(), 0, attr_bytes);
        CloseHandle(stdin_handle);
        CloseHandle(stdout_handle);
        if (stderr_file != INVALID_HANDLE_VALUE) {
            CloseHandle(stderr_file);
        }
        throw std::runtime_error("UpdateProcThreadAttribute failed");
    }

    STARTUPINFOEXA startup_ex{};
    startup_ex.StartupInfo.cb = sizeof(startup_ex);
    startup_ex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup_ex.StartupInfo.hStdInput = stdin_handle;
    startup_ex.StartupInfo.hStdOutput = stdout_handle;
    startup_ex.StartupInfo.hStdError = stderr_handle;
    startup_ex.lpAttributeList = attr_list;

    PROCESS_INFORMATION info{};
    std::vector<char> command_mutable(command.begin(), command.end());
    command_mutable.push_back('\0');

    const BOOL ok = CreateProcessA(
        nullptr,
        command_mutable.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        env_block.empty() ? nullptr : env_block.data(),
        nullptr,
        &startup_ex.StartupInfo,
        &info);

    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_bytes);
    CloseHandle(stdin_handle);
    CloseHandle(stdout_handle);
    if (stderr_file != INVALID_HANDLE_VALUE) {
        CloseHandle(stderr_file);
    }

    if (!ok) {
        throw std::runtime_error(
            "CreateProcess failed for '" + resolved_args[0] + "' (Win32 " +
            std::to_string(GetLastError()) + ")");
    }

    CloseHandle(info.hThread);
    process_info_ = info;
    process_info_.hThread = nullptr;
    started_ = true;
}

void WindowsChildProcess::stop() {
    if (!started_) {
        return;
    }
    if (running()) {
        TerminateProcess(process_info_.hProcess, 1);
        WaitForSingleObject(process_info_.hProcess, 5000);
    }
    close_handles();
}

bool WindowsChildProcess::running() const {
    if (!started_ || process_info_.hProcess == nullptr) {
        return false;
    }
    return WaitForSingleObject(process_info_.hProcess, 0) == WAIT_TIMEOUT;
}

void WindowsChildProcess::close_handles() {
    if (process_info_.hProcess != nullptr) {
        CloseHandle(process_info_.hProcess);
    }
    if (process_info_.hThread != nullptr) {
        CloseHandle(process_info_.hThread);
    }
    process_info_ = {};
    started_ = false;
}

} // namespace archstreamer

#endif // _WIN32
