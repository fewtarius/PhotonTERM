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
set(DEPS_PREFIX "${CMAKE_SOURCE_DIR}/3rdp/prefix")
if(EXISTS "${DEPS_PREFIX}/lib/libssh2.a" OR EXISTS "${DEPS_PREFIX}/lib64/libssh2.a")
    message(STATUS "Bundled deps found at ${DEPS_PREFIX}")
    list(PREPEND CMAKE_PREFIX_PATH "${DEPS_PREFIX}")
    set(USE_BUNDLED_DEPS ON)
else()
    message(STATUS "Bundled deps not found - using system packages")
    set(USE_BUNDLED_DEPS OFF)
endif()

# ── cmake module path ──────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ── Subdirectories ─────────────────────────────────────────────────────
add_subdirectory(src/photonterm)
