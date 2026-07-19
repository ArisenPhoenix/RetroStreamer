#pragma once

#if defined(_WIN32)
#error "Windows platform implementation has not been added yet"
#else
#include "common/platform/posix_process.hpp"
#include "common/platform/posix_socket.hpp"
#endif

namespace archstreamer {

#if !defined(_WIN32)
using PlatformChildProcess = PosixChildProcess;
using PlatformTcpStream = PosixTcpStream;
using PlatformTcpListener = PosixTcpListener;
using PlatformUdpSocket = PosixUdpSocket;
#endif

using ChildProcess = PlatformChildProcess;
using TcpStream = PlatformTcpStream;
using TcpListener = PlatformTcpListener;
using UdpSocket = PlatformUdpSocket;

} // namespace archstreamer
