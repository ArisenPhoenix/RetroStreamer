# ARCHSTREAMER_RUNTIME_CADENCE is set in the root CMakeLists.txt (file|db).

if(NOT ARCHSTREAMER_RUNTIME_CADENCE STREQUAL "file" AND
   NOT ARCHSTREAMER_RUNTIME_CADENCE STREQUAL "db")
    message(FATAL_ERROR "ARCHSTREAMER_RUNTIME_CADENCE must be 'file' or 'db' (got '${ARCHSTREAMER_RUNTIME_CADENCE}')")
endif()

if(ARCHSTREAMER_RUNTIME_CADENCE STREQUAL "db" AND WIN32)
    message(WARNING "ARCHSTREAMER_RUNTIME_CADENCE=db is not supported on Windows yet; using file")
    set(ARCHSTREAMER_RUNTIME_CADENCE "file")
endif()

set(ARCHSTREAMER_CADENCE_COMMON_SOURCES
    src/types.cpp
    src/protocol.cpp
    src/make_store.cpp
    src/user_auth.cpp
    src/instance_ops.cpp
)

add_library(archstreamer_cadence_file STATIC
    ${ARCHSTREAMER_CADENCE_COMMON_SOURCES}
    src/file_store.cpp
)
target_include_directories(archstreamer_cadence_file
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(archstreamer_cadence_file
    PUBLIC
        ARCHSTREAMER_RUNTIME_CADENCE_FILE=1
)
target_link_libraries(archstreamer_cadence_file
    PUBLIC
        archstreamer_core
        nlohmann_json::nlohmann_json
)

# SQLite is required for cadence=db and for the host game metadata store.
# Always expose a real target (`archstreamer_sqlite`) that parents can link.
# Linking parents directly to `PkgConfig::ARCHSTREAMER_SQLITE` breaks: that
# IMPORTED target is directory-scoped to this subdirectory (common on Windows
# when MSYS/Chocolatey pkg-config finds sqlite3.pc).
set(ARCHSTREAMER_CADENCE_SQLITE_TARGET "")

# Prefer FetchContent amalgamation on Windows — MSVC builds rarely have a usable
# sqlite3 via pkg-config, and when they do the imported target is often invisible.
if(NOT WIN32 AND PkgConfig_FOUND)
    pkg_check_modules(ARCHSTREAMER_SQLITE QUIET IMPORTED_TARGET sqlite3)
    if(ARCHSTREAMER_SQLITE_FOUND)
        message(STATUS "Found sqlite3: ${ARCHSTREAMER_SQLITE_VERSION}")
        add_library(archstreamer_sqlite INTERFACE)
        target_link_libraries(archstreamer_sqlite INTERFACE PkgConfig::ARCHSTREAMER_SQLITE)
        set(ARCHSTREAMER_CADENCE_SQLITE_TARGET archstreamer_sqlite)
    endif()
endif()
if(NOT ARCHSTREAMER_CADENCE_SQLITE_TARGET)
    message(STATUS "sqlite3 pkg-config not used; fetching amalgamation via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        sqlite_amalgamation
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
        URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sqlite_amalgamation)
    if(NOT EXISTS "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
        message(FATAL_ERROR
            "SQLite amalgamation missing after FetchContent.\n"
            "Allow network access once so CMake can download sqlite-amalgamation,\n"
            "or on Linux install libsqlite3-dev / sqlite-devel.")
    endif()
    add_library(archstreamer_sqlite3 STATIC
        "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c"
    )
    set_target_properties(archstreamer_sqlite3 PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(archstreamer_sqlite3
        PUBLIC
            "${sqlite_amalgamation_SOURCE_DIR}"
    )
    find_package(Threads REQUIRED)
    target_link_libraries(archstreamer_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    add_library(archstreamer_sqlite INTERFACE)
    target_link_libraries(archstreamer_sqlite INTERFACE archstreamer_sqlite3)
    set(ARCHSTREAMER_CADENCE_SQLITE_TARGET archstreamer_sqlite)
endif()
set(ARCHSTREAMER_SQLITE_TARGET "${ARCHSTREAMER_CADENCE_SQLITE_TARGET}" PARENT_SCOPE)

if(ARCHSTREAMER_RUNTIME_CADENCE STREQUAL "db")
    add_library(archstreamer_cadence_db STATIC
        ${ARCHSTREAMER_CADENCE_COMMON_SOURCES}
        src/file_store.cpp
        src/db_store.cpp
    )
    target_include_directories(archstreamer_cadence_db
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(archstreamer_cadence_db
        PUBLIC
            ARCHSTREAMER_RUNTIME_CADENCE_DB=1
    )
    target_link_libraries(archstreamer_cadence_db
        PUBLIC
            archstreamer_core
            nlohmann_json::nlohmann_json
            ${ARCHSTREAMER_CADENCE_SQLITE_TARGET}
    )

    add_executable(archstreamer_cadence
        src/sidecar_main.cpp
        src/sidecar_db.cpp
        src/types.cpp
        src/protocol.cpp
        src/file_store.cpp
        src/db_store.cpp
        src/user_auth.cpp
        src/instance_ops.cpp
    )
    # host_runner / GUI spawn a sibling binary named archstreamer_cadence.
    set_target_properties(archstreamer_cadence PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(archstreamer_cadence
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(archstreamer_cadence
        PRIVATE
            ARCHSTREAMER_RUNTIME_CADENCE_DB=1
    )
    target_link_libraries(archstreamer_cadence
        PRIVATE
            archstreamer_core
            nlohmann_json::nlohmann_json
            ${ARCHSTREAMER_CADENCE_SQLITE_TARGET}
    )

    set(ARCHSTREAMER_CADENCE_LINK_TARGET archstreamer_cadence_db PARENT_SCOPE)
    set(ARCHSTREAMER_CADENCE_SIDECAR_TARGET archstreamer_cadence PARENT_SCOPE)
else()
    set(ARCHSTREAMER_CADENCE_LINK_TARGET archstreamer_cadence_file PARENT_SCOPE)
    set(ARCHSTREAMER_CADENCE_SIDECAR_TARGET "" PARENT_SCOPE)
endif()

message(STATUS "  Runtime cadence:  ${ARCHSTREAMER_RUNTIME_CADENCE}")
