#include "common/platform/process_utils.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace archstreamer {
namespace {

HANDLE open_nul(DWORD access) {
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

// GUI-subsystem parents: std::system/_popen spawn a visible console per call.
// Always use CREATE_NO_WINDOW (+ hidden show flag) for helper probes.
int run_hidden_command(const char* command, std::string* captured_stdout) {
    if (command == nullptr || command[0] == '\0') {
        return -1;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdin_handle = open_nul(GENERIC_READ);
    HANDLE stdout_read = INVALID_HANDLE_VALUE;
    HANDLE stdout_write = INVALID_HANDLE_VALUE;
    HANDLE stderr_write = INVALID_HANDLE_VALUE;

    if (stdin_handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (captured_stdout != nullptr) {
        if (!CreatePipe(&stdout_read, &stdout_write, &security, 0)) {
            CloseHandle(stdin_handle);
            return -1;
        }
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
        stderr_write = stdout_write;
    } else {
        stdout_write = open_nul(GENERIC_WRITE);
        stderr_write = stdout_write;
        if (stdout_write == INVALID_HANDLE_VALUE) {
            CloseHandle(stdin_handle);
            return -1;
        }
    }

    std::string cmdline = std::string("cmd.exe /C ") + command;
    std::vector<char> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = stdin_handle;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;

    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessA(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &info);

    CloseHandle(stdin_handle);
    CloseHandle(stdout_write);
    if (captured_stdout == nullptr && stderr_write != stdout_write &&
        stderr_write != INVALID_HANDLE_VALUE) {
        CloseHandle(stderr_write);
    }

    if (!ok) {
        if (stdout_read != INVALID_HANDLE_VALUE) {
            CloseHandle(stdout_read);
        }
        return -1;
    }

    CloseHandle(info.hThread);

    if (captured_stdout != nullptr) {
        captured_stdout->clear();
        char buffer[512];
        DWORD read = 0;
        while (ReadFile(stdout_read, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            captured_stdout->append(buffer, buffer + read);
        }
        CloseHandle(stdout_read);
    }

    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
    return static_cast<int>(exit_code);
}

} // namespace

std::string trim_ascii_whitespace(std::string value) {
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() &&
           (value[start] == '\n' || value[start] == '\r' || value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        value.erase(0, start);
    }
    return value;
}

std::string read_command_output(const char* command) {
    std::string output;
    if (run_hidden_command(command, &output) < 0) {
        return {};
    }
    return trim_ascii_whitespace(std::move(output));
}

int run_command_exit_code(const char* command) {
    return run_hidden_command(command, nullptr);
}

void terminate_gst_multiudpsink_on_port(std::uint16_t port) {
    (void)port;
}

} // namespace archstreamer
