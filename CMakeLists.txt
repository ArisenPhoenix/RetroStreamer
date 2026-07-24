cmake_minimum_required(VERSION 3.20)

project(ArchStreamer LANGUAGES CXX)

include(FetchContent)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
  "Export compile_commands.json in this build directory."
  FORCE
)

set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_dependencies")

if(WIN32)
    option(ARCHSTREAMER_BUILD_HOST "Build host runtime (Linux only today)" OFF)
else()
    option(ARCHSTREAMER_BUILD_HOST "Build host runtime (Linux only today)" ON)
endif()
option(ARCHSTREAMER_REQUIRE_GUI "Fail configure if Qt6 Widgets is missing" OFF)
option(ARCHSTREAMER_BUILD_CONTROLLER_PROBE "Build controller probe tool" ON)

if(ARCHSTREAMER_BUILD_HOST AND WIN32)
    message(FATAL_ERROR
        "Host runtime is not available on Windows yet.\n"
        "Configure with -DARCHSTREAMER_BUILD_HOST=OFF for a client-capable build.")
endif()

function(archstreamer_require_pkgconfig_module RESULT_PREFIX MODULE_NAME)
    pkg_check_modules(${RESULT_PREFIX} IMPORTED_TARGET ${MODULE_NAME})
    if(NOT ${RESULT_PREFIX}_FOUND)
        set(_hint "")
        foreach(_part IN LISTS ARGN)
            string(APPEND _hint "${_part}")
        endforeach()
        message(FATAL_ERROR
            "Missing dependency: pkg-config module '${MODULE_NAME}'.\n"
            "${_hint}\n"
            "After installing, clear the build dir and re-run cmake.")
    endif()
    message(STATUS "Found ${MODULE_NAME}: ${${RESULT_PREFIX}_VERSION}")
endfunction()

find_package(PkgConfig)
if(NOT PkgConfig_FOUND AND NOT WIN32)
    message(FATAL_ERROR
        "Missing dependency: PkgConfig (pkg-config).\n"
        "  Fedora/Bazzite (distrobox): sudo dnf install pkgconf-pkg-config\n"
        "  Debian/Ubuntu:              sudo apt install pkg-config\n"
        "  Arch:                       sudo pacman -S pkgconf")
endif()

# Prefer a preinstalled package (Flatpak module / distro). FetchContent needs
# network and fails in sandboxed builds that cannot git-clone at configure time.
find_package(nlohmann_json 3.11.0 QUIET)
if(nlohmann_json_FOUND)
    message(STATUS "Found nlohmann_json: ${nlohmann_json_VERSION}")
else()
    message(STATUS "nlohmann_json not found; fetching v3.12.0 via FetchContent")
    FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.12.0
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6Widgets_FOUND)
    message(STATUS "Found Qt6 Widgets: ${Qt6Widgets_VERSION}")
else()
    set(_qt_hint
        "Qt6 Widgets not found; archstreamer_gui will be skipped.\n"
        "  Fedora/Bazzite (distrobox): sudo dnf install qt6-qtbase-devel\n"
        "  Debian/Ubuntu:              sudo apt install qt6-base-dev\n"
        "  Arch:                       sudo pacman -S qt6-base\n"
        "  Or build the Flatpak under deploy/flatpak/.")
    if(ARCHSTREAMER_REQUIRE_GUI)
        message(FATAL_ERROR ${_qt_hint})
    else()
        message(WARNING ${_qt_hint})
    endif()
endif()

set(ARCHSTREAMER_CORE_SOURCES
    src/common/addresses.cpp
    src/common/art_transfer.cpp
    src/common/catalog_presenter.cpp
    src/common/cli_common.cpp
    src/common/controller_normalization.cpp
    src/common/discovery.cpp
    src/common/game_assets.cpp
    src/common/steam_art_import.cpp
    src/common/participant_role.cpp
    src/common/platform/paths.cpp
    src/common/platform/process_utils.cpp
    src/common/protocol.cpp
    src/common/serialization.cpp
    src/common/sha256.cpp
    src/common/time.cpp
)

if(WIN32)
    list(APPEND ARCHSTREAMER_CORE_SOURCES
        src/common/platform/windows_process.cpp
        src/common/platform/windows_socket.cpp
    )
else()
    list(APPEND ARCHSTREAMER_CORE_SOURCES
        src/common/platform/posix_process.cpp
        src/common/platform/posix_socket.cpp
    )
endif()

add_library(archstreamer_core ${ARCHSTREAMER_CORE_SOURCES})
target_include_directories(archstreamer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(archstreamer_core PUBLIC nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(archstreamer_core PUBLIC ws2_32 iphlpapi)
endif()

set(ARCHSTREAMER_CONTROLLER_BACKEND "sdl2" CACHE STRING "Controller backend implementation")
set_property(CACHE ARCHSTREAMER_CONTROLLER_BACKEND PROPERTY STRINGS sdl2)

if(ARCHSTREAMER_CONTROLLER_BACKEND STREQUAL "sdl2")
    set(ARCHSTREAMER_SDL2_TARGET "")
    if(WIN32)
        find_package(SDL2 QUIET)
        if(SDL2_FOUND)
            if(TARGET SDL2::SDL2)
                set(ARCHSTREAMER_SDL2_TARGET SDL2::SDL2)
            elseif(TARGET SDL2::SDL2-static)
                set(ARCHSTREAMER_SDL2_TARGET SDL2::SDL2-static)
            endif()
            if(DEFINED SDL2_VERSION)
                message(STATUS "Found SDL2 via CMake: ${SDL2_VERSION}")
            else()
                message(STATUS "Found SDL2 via CMake")
            endif()
        endif()
    endif()

    if(NOT ARCHSTREAMER_SDL2_TARGET)
        if(NOT PkgConfig_FOUND)
            message(FATAL_ERROR
                "SDL2 not found.\n"
                "  Windows: install SDL2 and set SDL2_DIR, or provide pkg-config.\n"
                "  Linux:   sudo install SDL2 devel packages (see hints below).")
        endif()
        archstreamer_require_pkgconfig_module(
            SDL2
            sdl2
            "  Fedora/Bazzite (distrobox): sudo dnf install SDL2-devel\n"
            "  Debian/Ubuntu:              sudo apt install libsdl2-dev\n"
            "  Arch:                       sudo pacman -S sdl2\n"
            "  Verify:                     pkg-config --exists sdl2 && pkg-config --modversion sdl2")
        set(ARCHSTREAMER_SDL2_TARGET PkgConfig::SDL2)
    endif()

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
            ${ARCHSTREAMER_SDL2_TARGET}
    )
else()
    message(FATAL_ERROR "Unsupported ARCHSTREAMER_CONTROLLER_BACKEND: ${ARCHSTREAMER_CONTROLLER_BACKEND}")
endif()

add_library(archstreamer_client
    src/client/catalog_cache.cpp
    src/client/client_app.cpp
    src/client/controller_manager.cpp
    src/client/game_filter.cpp
    src/client/gstreamer_probe.cpp
    src/client/audio_playback_device.cpp
    src/client/gstreamer_media_pipeline.cpp
    src/client/gstreamer_media_receiver.cpp
    src/client/gstreamer_synced_media_session.cpp
    src/client/input_sender.cpp
    src/client/session_client.cpp
    src/client/session_service.cpp
)
if(WIN32)
    target_sources(archstreamer_client PRIVATE
        src/client/windows_gstreamer_media_platform.cpp
    )
else()
    target_sources(archstreamer_client PRIVATE
        src/client/posix_gstreamer_media_platform.cpp
    )
endif()
target_link_libraries(archstreamer_client
    PUBLIC
        archstreamer_controller
)

set(ARCHSTREAMER_INSTALL_TARGETS
    session_client
    client_catalog_probe
)

if(ARCHSTREAMER_BUILD_HOST)
    add_library(archstreamer_host
        src/host/game_catalog.cpp
        src/host/game_catalog_scanner.cpp
        src/host/gpu_select.cpp
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
        src/host/retroarch_resolve.cpp
        src/host/save_profile.cpp
        src/host/seat_manager.cpp
        src/host/session_control_monitor.cpp
        src/host/session_lobby.cpp
        src/host/session_service.cpp
        src/host/virtual_joypad_resolve.cpp
    )
    target_compile_definitions(archstreamer_host PUBLIC ARCHSTREAMER_HAS_HOST=1)
    target_link_libraries(archstreamer_host
        PUBLIC
            archstreamer_core
        PRIVATE
            archstreamer_controller
    )

    add_executable(game_catalog_probe
        src/tools/game_catalog_probe.cpp
    )
    target_link_libraries(game_catalog_probe PRIVATE archstreamer_host)

    add_executable(asset_probe
        src/tools/asset_probe.cpp
    )
    target_link_libraries(asset_probe PRIVATE archstreamer_host)

    add_executable(steam_art_import
        src/tools/steam_art_import.cpp
    )
    target_link_libraries(steam_art_import PRIVATE archstreamer_host)

    add_executable(uinput_probe
        src/tools/uinput_probe.cpp
    )
    target_link_libraries(uinput_probe PRIVATE archstreamer_host)

    add_executable(host_runner
        src/tools/cli_host.cpp
        src/tools/host_runner.cpp
        src/tools/host_runner_main.cpp
    )
    target_link_libraries(host_runner PRIVATE archstreamer_host)

    list(APPEND ARCHSTREAMER_INSTALL_TARGETS
        game_catalog_probe
        asset_probe
        steam_art_import
        uinput_probe
        host_runner
    )
endif()

if(ARCHSTREAMER_BUILD_CONTROLLER_PROBE)
    add_executable(controller_probe
        src/tools/controller_probe.cpp
    )
    target_link_libraries(controller_probe
        PRIVATE
            archstreamer_controller
    )
    list(APPEND ARCHSTREAMER_INSTALL_TARGETS controller_probe)
endif()

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

# Client GUI always when Qt is present; Host tab only when ARCHSTREAMER_BUILD_HOST=ON.
if(Qt6Widgets_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_executable(archstreamer_gui
        src/gui/game_picker_widget.cpp
        src/gui/game_picker_widget.hpp
        src/gui/game_selection_dialog.cpp
        src/gui/game_selection_dialog.hpp
        src/gui/host_picker_widget.cpp
        src/gui/host_picker_widget.hpp
        src/gui/host_search_dialog.cpp
        src/gui/host_search_dialog.hpp
        src/tools/archstreamer_gui.cpp
    )
    target_include_directories(archstreamer_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gui)
    target_link_libraries(archstreamer_gui
        PRIVATE
            archstreamer_client
            Qt6::Widgets
    )
    if(ARCHSTREAMER_BUILD_HOST)
        target_link_libraries(archstreamer_gui PRIVATE archstreamer_host)
    endif()
    if(WIN32)
        # Keep SDL2 next to the GUI for controller listing / sessions.
        if(TARGET SDL2::SDL2)
            add_custom_command(TARGET archstreamer_gui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:SDL2::SDL2>
                    $<TARGET_FILE_DIR:archstreamer_gui>
                VERBATIM
            )
        endif()
    endif()
    list(APPEND ARCHSTREAMER_INSTALL_TARGETS archstreamer_gui)
endif()

install(TARGETS ${ARCHSTREAMER_INSTALL_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

message(STATUS "")
message(STATUS "ArchStreamer dependency summary")
message(STATUS "  C++ standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build host:       ${ARCHSTREAMER_BUILD_HOST}")
message(STATUS "  Controller:       ${ARCHSTREAMER_CONTROLLER_BACKEND}")
if(DEFINED SDL2_VERSION)
    message(STATUS "  SDL2:             ${SDL2_VERSION}")
else()
    message(STATUS "  SDL2:             found")
endif()
if(Qt6Widgets_FOUND)
    message(STATUS "  Qt6 Widgets:      ${Qt6Widgets_VERSION}")
    if(ARCHSTREAMER_BUILD_HOST)
        message(STATUS "  GUI target:       yes (archstreamer_gui, client+host tabs)")
    else()
        message(STATUS "  GUI target:       yes (archstreamer_gui, client-only)")
    endif()
else()
    message(STATUS "  Qt6 Widgets:      not found")
    message(STATUS "  GUI target:       no")
endif()
message(STATUS "  Require GUI:      ${ARCHSTREAMER_REQUIRE_GUI}")
message(STATUS "")
