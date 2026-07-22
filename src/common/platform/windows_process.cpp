#include "common/platform/windows_process.hpp"

#ifdef _WIN32

#include <stdexcept>
#include <utility>

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

    auto command = join_command_line(args);
    auto env_block = build_environment_block(env, unset_env);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    HANDLE stderr_handle = INVALID_HANDLE_VALUE;
    if (!stderr_path.empty()) {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        stderr_handle = CreateFileA(
            stderr_path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            &security,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (stderr_handle == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("failed to open child stderr path");
        }
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = stderr_handle;
    }

    PROCESS_INFORMATION info{};
    std::vector<char> command_mutable(command.begin(), command.end());
    command_mutable.push_back('\0');

    const BOOL ok = CreateProcessA(
        nullptr,
        command_mutable.data(),
        nullptr,
        nullptr,
        stderr_handle != INVALID_HANDLE_VALUE ? TRUE : FALSE,
        0,
        env_block.empty() ? nullptr : env_block.data(),
        nullptr,
        &startup,
        &info);

    if (stderr_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(stderr_handle);
    }

    if (!ok) {
        throw std::runtime_error("CreateProcess failed");
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
