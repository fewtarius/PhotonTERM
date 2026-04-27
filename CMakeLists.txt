cmake_minimum_required(VERSION 3.20)

# Only enable Objective-C on Apple platforms (DarwinWrappers.m)
if(APPLE)
    project(PhotonTERM
        VERSION 0.1.0
        DESCRIPTION "PhotonTERM - BBS terminal client"
        LANGUAGES C OBJC)
else()
    project(PhotonTERM
        VERSION 0.1.0
        DESCRIPTION "PhotonTERM - BBS terminal client"
        LANGUAGES C)
endif()

# ── Require C17 ────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Default build type ─────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

# ── Bundled deps prefix (3rdp/prefix from build-deps.sh) ──────────────
# The bundled deps are built per-platform by build-deps.sh.  On macOS
# they include CMake configs with FRAMEWORK link features; on Linux
# they include pkg-config files.  Only use bundled deps on their
# native platform.  On non-Apple, if the bundled libs are Mach-O
# (macOS-built), fall back to system packages.
set(DEPS_PREFIX "${CMAKE_SOURCE_DIR}/3rdp/prefix")
if(APPLE AND EXISTS "${DEPS_PREFIX}/lib/libssh2.a")
    message(STATUS "Bundled deps found at ${DEPS_PREFIX}")
    list(PREPEND CMAKE_PREFIX_PATH "${DEPS_PREFIX}")
    set(USE_BUNDLED_DEPS ON)
elseif(NOT APPLE AND EXISTS "${DEPS_PREFIX}/lib/libssh2.a")
    # Check if bundled deps are native (ELF) or foreign (Mach-O).
    # CMake's find_package will fail with FRAMEWORK errors on Linux
    # if the deps were built on macOS.
    execute_process(
        COMMAND file "${DEPS_PREFIX}/lib/libSDL2.a"
        OUTPUT_VARIABLE _DEPS_FILE_TYPE
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_DEPS_FILE_TYPE MATCHES "ELF")
        message(STATUS "Bundled deps found at ${DEPS_PREFIX}")
        list(PREPEND CMAKE_PREFIX_PATH "${DEPS_PREFIX}")
        set(USE_BUNDLED_DEPS ON)
    else()
        message(STATUS "Bundled deps are not native - using system packages")
        message(STATUS "  (Run scripts/build-deps.sh to build native deps)")
        set(USE_BUNDLED_DEPS OFF)
    endif()
else()
    message(STATUS "Bundled deps not found - using system packages")
    set(USE_BUNDLED_DEPS OFF)
endif()

# ── cmake module path ──────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ── Subdirectories ─────────────────────────────────────────────────────
add_subdirectory(src/photonterm)
