#pragma once

#if defined(_WIN32)
#include "common/platform/windows_process.hpp"
#include "common/platform/windows_socket.hpp"
#else
#include "common/platform/posix_process.hpp"
#include "common/platform/posix_socket.hpp"
#endif

namespace archstreamer {

#if defined(_WIN32)
using PlatformChildProcess = WindowsChildProcess;
using PlatformTcpStream = WindowsTcpStream;
using PlatformTcpListener = WindowsTcpListener;
using PlatformUdpSocket = WindowsUdpSocket;
#else
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
