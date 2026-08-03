#pragma once

#if defined(_WIN32)
#include "host/windows_save_profile_paths.hpp"
#endif

namespace archstreamer {

#if defined(_WIN32)
using SaveProfilePaths = WindowsSaveProfilePaths;
#endif
// Non-Windows: free functions in save_profile.cpp remain the reference
// implementation. A PosixSaveProfilePaths alias will wrap them once that
// interface is settled without changing behaviour.

} // namespace archstreamer
