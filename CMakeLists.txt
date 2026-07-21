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

find_package(Qt6 COMPONENTS Widgets QUIET)

add_library(archstreamer_core
    src/common/addresses.cpp
    src/common/catalog_presenter.cpp
    src/common/cli_common.cpp
    src/common/controller_normalization.cpp
    src/common/discovery.cpp
    src/common/game_assets.cpp
    src/common/steam_art_import.cpp
    src/common/participant_role.cpp
    src/common/platform/posix_process.cpp
    src/common/platform/posix_socket.cpp
    src/common/platform/process_utils.cpp
    src/common/protocol.cpp
    src/common/serialization.cpp
    src/common/sha256.cpp
    src/common/time.cpp
)
target_include_directories(archstreamer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(archstreamer_core PUBLIC nlohmann_json::nlohmann_json)

add_library(archstreamer_host
    src/host/game_catalog.cpp
    src/host/game_catalog_scanner.cpp
    src/host/gstreamer_media_server.cpp
    src/host/host_app.cpp
    src/host/host_app_config.cpp
    src/host/host_launch_planner.cpp
    src/host/host_session_helpers.cpp
    src/host/input_router.cpp
    src/host/libretro_core_registry.cpp
    src/host/linux_uinput_gamepad.cpp
    src/host/local_controller_bridge.cpp
    src/host/network_input_receiver.cpp
    src/host/posix_retroarch_process.cpp
    src/host/retroarch_config_writer.cpp
    src/host/save_profile.cpp
    src/host/seat_manager.cpp
    src/host/session_control_monitor.cpp
    src/host/session_lobby.cpp
    src/host/session_service.cpp
)
target_link_libraries(archstreamer_host
    PUBLIC
        archstreamer_core
    PRIVATE
        archstreamer_controller
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

add_executable(steam_art_import
    src/tools/steam_art_import.cpp
)
target_link_libraries(steam_art_import
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
    src/tools/cli_host.cpp
    src/tools/host_runner.cpp
    src/tools/host_runner_main.cpp
)
target_link_libraries(host_runner
    PRIVATE
        archstreamer_host
)

add_executable(session_client
    src/tools/cli_client.cpp
    src/tools/session_client_app.cpp
    src/tools/session_client_main.cpp
)
target_link_libraries(session_client
    PRIVATE
        archstreamer_client
)

add_executable(client_catalog_probe
    src/tools/client_catalog_probe.cpp
)
target_link_libraries(client_catalog_probe
    PRIVATE
        archstreamer_client
)

if(Qt6Widgets_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_executable(archstreamer_gui
        src/gui/game_picker_widget.cpp
        src/gui/game_picker_widget.hpp
        src/gui/game_selection_dialog.cpp
        src/gui/game_selection_dialog.hpp
        src/gui/host_picker_widget.cpp
        src/gui/host_picker_widget.hpp
        src/tools/archstreamer_gui.cpp
    )
    target_include_directories(archstreamer_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gui)
    target_link_libraries(archstreamer_gui
        PRIVATE
            archstreamer_client
            archstreamer_host
            Qt6::Widgets
    )
endif()
