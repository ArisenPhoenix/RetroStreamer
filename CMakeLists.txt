cmake_minimum_required(VERSION 3.20)

project(ArchStreamer LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
  "Export compile_commands.json in this build directory."
  FORCE
)

set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_dependencies")

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(archstreamer_core
    src/common/controller_normalization.cpp
    src/common/game_assets.cpp
    src/common/protocol.cpp
    src/common/sha256.cpp
    src/common/time.cpp
)
target_include_directories(archstreamer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(archstreamer_core PUBLIC nlohmann_json::nlohmann_json)

add_library(archstreamer_host
    src/host/linux_uinput_gamepad.cpp
    src/host/posix_retroarch_process.cpp
)
target_link_libraries(archstreamer_host
    PUBLIC
        archstreamer_core
)

set(ARCHSTREAMER_CONTROLLER_BACKEND "sdl2" CACHE STRING "Controller backend implementation")
set_property(CACHE ARCHSTREAMER_CONTROLLER_BACKEND PROPERTY STRINGS sdl2)

if(ARCHSTREAMER_CONTROLLER_BACKEND STREQUAL "sdl2")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

    add_library(archstreamer_controller
        src/client/sdl2_controller_backend.cpp
    )
    target_compile_definitions(archstreamer_controller
        PUBLIC
            ARCHSTREAMER_CONTROLLER_BACKEND_SDL2
    )
    target_link_libraries(archstreamer_controller
        PUBLIC
            archstreamer_core
            PkgConfig::SDL2
    )
else()
    message(FATAL_ERROR "Unsupported ARCHSTREAMER_CONTROLLER_BACKEND: ${ARCHSTREAMER_CONTROLLER_BACKEND}")
endif()

add_library(archstreamer_client
    src/client/catalog_cache.cpp
    src/client/client_app.cpp
    src/client/controller_manager.cpp
    src/client/game_filter.cpp
    src/client/gstreamer_media_receiver.cpp
    src/client/gui.cpp
    src/client/input_sender.cpp
    src/client/session_client.cpp
    src/client/session_service.cpp
)
target_link_libraries(archstreamer_client
    PUBLIC
        archstreamer_controller
)

option(ARCHSTREAMER_BUILD_CONTROLLER_PROBE "Build controller probe tool" ON)

if(ARCHSTREAMER_BUILD_CONTROLLER_PROBE)
    add_executable(controller_probe
        src/tools/controller_probe.cpp
    )
    target_link_libraries(controller_probe
        PRIVATE
            archstreamer_controller
    )
endif()

add_executable(game_catalog_probe
    src/tools/game_catalog_probe.cpp
)
target_link_libraries(game_catalog_probe
    PRIVATE
        archstreamer_host
)

add_executable(asset_probe
    src/tools/asset_probe.cpp
)
target_link_libraries(asset_probe
    PRIVATE
        archstreamer_host
)

add_executable(uinput_probe
    src/tools/uinput_probe.cpp
)
target_link_libraries(uinput_probe
    PRIVATE
        archstreamer_host
)

add_executable(host_runner
    src/tools/host_runner.cpp
)
target_link_libraries(host_runner
    PRIVATE
        archstreamer_host
        archstreamer_controller
)

add_executable(session_client
    src/tools/session_client.cpp
    src/tools/session_client_cli.cpp
)
target_link_libraries(session_client
    PRIVATE
        archstreamer_client
)
