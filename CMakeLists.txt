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
    option(ARCHSTREAMER_BUILD_HOST "Build host runtime (Windows: Yuzu + DXGI/WASAPI/ViGEm)" OFF)
else()
    option(ARCHSTREAMER_BUILD_HOST "Build host runtime (Linux)" ON)
endif()
option(ARCHSTREAMER_REQUIRE_GUI "Fail configure if Qt6 Widgets is missing" OFF)
option(ARCHSTREAMER_BUILD_CONTROLLER_PROBE "Build controller probe tool" ON)
# Experimental DoubleCherryGB dual-machine cable (no GB/GBC saves to test with yet).
option(ARCHSTREAMER_DEBUG_GB_LINK "Enable experimental GB/GBC dual-GB link backend" OFF)

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
    src/common/protocol.cpp
    src/common/serialization.cpp
    src/common/sha256.cpp
    src/common/time.cpp
)

if(WIN32)
    list(APPEND ARCHSTREAMER_CORE_SOURCES
        src/common/platform/windows_discovery_net.cpp
        src/common/platform/windows_paths.cpp
        src/common/platform/windows_process.cpp
        src/common/platform/windows_process_utils.cpp
        src/common/platform/windows_socket.cpp
    )
else()
    list(APPEND ARCHSTREAMER_CORE_SOURCES
        src/common/platform/posix_discovery_net.cpp
        src/common/platform/posix_paths.cpp
        src/common/platform/posix_process.cpp
        src/common/platform/posix_process_utils.cpp
        src/common/platform/posix_socket.cpp
    )
endif()

add_library(archstreamer_core ${ARCHSTREAMER_CORE_SOURCES})
target_include_directories(archstreamer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(ARCHSTREAMER_DEBUG_GB_LINK)
    target_compile_definitions(archstreamer_core PUBLIC ARCHSTREAMER_DEBUG_GB_LINK=1)
endif()
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
    src/client/client_media_playback.cpp
    src/client/controller_manager.cpp
    src/client/game_filter.cpp
    src/client/gstreamer_probe.cpp
    src/client/audio_playback_device.cpp
    src/client/gstreamer_media_pipeline.cpp
    src/client/gstreamer_media_receiver.cpp
    src/client/gstreamer_synced_media_session.cpp
    src/client/input_sender.cpp
    src/client/keyboard_poller.cpp
    src/client/remoted_keyboard_source.cpp
    src/client/session_client.cpp
    src/client/session_service.cpp
)
if(WIN32)
    target_sources(archstreamer_client PRIVATE
        src/client/windows_audio_playback_device.cpp
        src/client/windows_gstreamer_media_platform.cpp
        src/client/windows_video_window_geometry.cpp
        src/client/win32_remoted_keyboard_source.cpp
    )
else()
    target_sources(archstreamer_client PRIVATE
        src/client/posix_audio_playback_device.cpp
        src/client/posix_gstreamer_media_platform.cpp
        src/client/posix_video_window_geometry.cpp
        src/client/evdev_remoted_keyboard_source.cpp
        src/client/x11_keymap_remoted_keyboard_source.cpp
    )
    find_package(X11 REQUIRED)
    target_link_libraries(archstreamer_client PUBLIC X11::X11)
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
    set(ARCHSTREAMER_HOST_SOURCES
        src/host/game_catalog.cpp
        src/host/game_catalog_scanner.cpp
        src/host/gpu_select.cpp
        src/host/host_app.cpp
        src/host/host_app_config.cpp
        src/host/host_concurrent_lobby.cpp
        src/host/host_launch_planner.cpp
        src/host/host_session_helpers.cpp
        src/host/host_session_hub.cpp
        src/host/input_router.cpp
        src/host/input_router_demux.cpp
        src/host/active_session_slot.cpp
        src/host/launch_environment.cpp
        src/host/libretro_core_registry.cpp
        src/host/virtual_keyboard.cpp
        src/host/local_controller_bridge.cpp
        src/host/network_input_receiver.cpp
        src/host/streaming_audio_sink.cpp
        src/host/retroarch_config_writer.cpp
        src/host/retroarch_netcmd.cpp
        src/host/retroarch_resolve.cpp
        src/host/save_profile.cpp
        src/host/standalone_emulator.cpp
        src/host/switch/switch_fs.cpp
        src/host/switch/switch_system_defaults.cpp
        src/host/switch/qt_ini_editor.cpp
        src/host/switch/yuzu_user_profile.cpp
        src/host/switch/yuzu_controls.cpp
        src/host/switch/yuzu_launch_env.cpp
        src/host/switch/ryujinx_user_profile.cpp
        src/host/switch/ryujinx_controls.cpp
        src/host/switch/ryujinx_launch_env.cpp
        src/host/switch/switch_backend.cpp
        src/host/switch/ryujinx_backend.cpp
        src/host/switch/yuzu_backend.cpp
        src/host/switch/switch_runtime.cpp
        src/host/switch_save_share.cpp
        src/host/link_coordinator.cpp
        src/host/link_cable_backend.cpp
        src/host/seat_manager.cpp
        src/host/session_control_monitor.cpp
        src/host/session_launch_assemble.cpp
        src/host/session_run_helpers.cpp
        src/host/session_lobby.cpp
        src/host/session_runtime.cpp
        src/host/session_slot_lease.cpp
        src/host/virtual_joypad_resolve.cpp
    )
    if(WIN32)
        list(APPEND ARCHSTREAMER_HOST_SOURCES
            src/host/windows_launch_environment.cpp
            src/host/windows_session_slot_lease.cpp
            src/host/windows_retroarch_process.cpp
            src/host/windows_vigem_gamepad.cpp
            src/host/windows_media_server.cpp
            src/host/windows_capture_platform.cpp
            src/host/windows_emulator_orphan_reaper.cpp
            src/host/switch/windows_switch_paths.cpp
            src/host/switch/windows_yuzu_runtime.cpp
            src/host/switch/windows_ryujinx_runtime.cpp
        )
    else()
        list(APPEND ARCHSTREAMER_HOST_SOURCES
            src/host/posix_launch_environment.cpp
            src/host/posix_session_slot_lease.cpp
            src/host/gstreamer_media_server.cpp
            src/host/linux_uinput_gamepad.cpp
            src/host/posix_retroarch_process.cpp
            src/host/virtual_display.cpp
            src/host/linux_capture_platform.cpp
            src/host/linux_emulator_orphan_reaper.cpp
            src/host/switch/posix_switch_paths.cpp
            src/host/switch/posix_yuzu_runtime.cpp
            src/host/switch/posix_ryujinx_runtime.cpp
        )
    endif()

    add_library(archstreamer_host ${ARCHSTREAMER_HOST_SOURCES})
    target_compile_definitions(archstreamer_host PUBLIC ARCHSTREAMER_HAS_HOST=1)
    target_link_libraries(archstreamer_host
        PUBLIC
            archstreamer_core
        PRIVATE
            archstreamer_controller
    )
    if(NOT WIN32)
        find_package(X11 REQUIRED)
        if(NOT TARGET X11::Xtst AND NOT X11_Xtst_FOUND)
            find_path(X11_Xtst_INCLUDE_PATH NAMES X11/extensions/XTest.h)
            find_library(X11_Xtst_LIB NAMES Xtst)
        endif()
        target_link_libraries(archstreamer_host PRIVATE X11::X11)
        if(TARGET X11::Xtst)
            target_link_libraries(archstreamer_host PRIVATE X11::Xtst)
        elseif(X11_Xtst_LIB)
            target_link_libraries(archstreamer_host PRIVATE ${X11_Xtst_LIB})
            if(X11_Xtst_INCLUDE_PATH)
                target_include_directories(archstreamer_host PRIVATE ${X11_Xtst_INCLUDE_PATH})
            endif()
        else()
            message(FATAL_ERROR "libXtst required for host virtual keyboard (apt: libxtst-dev)")
        endif()
    endif()

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

    if(NOT WIN32)
        add_executable(uinput_probe
            src/tools/uinput_probe.cpp
        )
        target_link_libraries(uinput_probe PRIVATE archstreamer_host)
        list(APPEND ARCHSTREAMER_INSTALL_TARGETS uinput_probe)
    endif()

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
    # WIN32 = Windows GUI subsystem (no console window). Logs still go to
    # %TEMP%\archstreamer-logs\gui.log. Omit WIN32 only if you need stdout.
    set(ARCHSTREAMER_GUI_SOURCES
        src/gui/game_picker_widget.cpp
        src/gui/game_picker_widget.hpp
        src/gui/game_selection_dialog.cpp
        src/gui/game_selection_dialog.hpp
        src/gui/host_picker_widget.cpp
        src/gui/host_picker_widget.hpp
        src/gui/host_search_dialog.cpp
        src/gui/host_search_dialog.hpp
        src/gui/pad_on_screen_keyboard.cpp
        src/gui/pad_on_screen_keyboard.hpp
        src/gui/gui_logging.cpp
        src/gui/gui_logging.hpp
        src/gui/gui_util.cpp
        src/gui/gui_util.hpp
        src/gui/main_window.hpp
        src/gui/main_window.cpp
        src/gui/main_window_client.cpp
        src/gui/main_window_settings.cpp
        src/gui/main_window_game_options.cpp
        src/tools/archstreamer_gui.cpp
    )
    if(ARCHSTREAMER_BUILD_HOST)
        list(APPEND ARCHSTREAMER_GUI_SOURCES
            src/gui/gui_host_runner.cpp
            src/gui/gui_host_runner.hpp
            src/gui/main_window_host.cpp
        )
    endif()
    add_executable(archstreamer_gui ${ARCHSTREAMER_GUI_SOURCES})
    if(WIN32)
        set_target_properties(archstreamer_gui PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
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
message(STATUS "  Debug GB link:    ${ARCHSTREAMER_DEBUG_GB_LINK}")
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
