#pragma once

#if defined(ARCHSTREAMER_CONTROLLER_BACKEND_SDL2)
#include "client/sdl2_controller_backend.hpp"
#else
#error "No ArchStreamer controller backend selected"
#endif

namespace archstreamer {

#if defined(ARCHSTREAMER_CONTROLLER_BACKEND_SDL2)
using ControllerBackend = Sdl2ControllerBackend;
#endif

} // namespace archstreamer
