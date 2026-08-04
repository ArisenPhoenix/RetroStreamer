// Minimal OpenSSH SSH_ASKPASS helper for Windows.
// Prints ARCHSTREAMER_SSH_PASSWORD to stdout and exits.
// OpenSSH on Windows requires a real PE executable for SSH_ASKPASS
// (shell/.cmd scripts fail with CreateProcessW error 193).

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

int main() {
#ifdef _WIN32
    // Avoid a flash console stealing focus when OpenSSH spawns askpass.
    FreeConsole();
#endif
    const char* password = std::getenv("ARCHSTREAMER_SSH_PASSWORD");
    if (password == nullptr) {
        password = "";
    }
    std::cout << password << '\n';
    std::cout.flush();
    // Best-effort scrub of the local copy only; parent clears the env after ssh.
    if (password[0] != '\0') {
        // getenv returns a pointer into the environment block; do not write to it.
    }
    return 0;
}
