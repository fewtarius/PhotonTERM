# src/photonterm/CMakeLists.txt - PhotonTERM main build target
#
# Dependencies (resolved via CMAKE_PREFIX_PATH -> 3rdp/prefix if bundled):
#   SDL2, SDL2_ttf, OpenSSL, libssh2, Threads, ZLIB
#
# macOS only: Foundation framework, Objective-C source (DarwinWrappers.m)
# Linux/other: early_init.c (constructor attribute for pre-SDL signal setup)

# ── Find dependencies ──────────────────────────────────────────────────
find_package(SDL2 REQUIRED)
find_package(SDL2_ttf REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# libssh2 ships a cmake config in lib/cmake/libssh2/
find_package(libssh2 REQUIRED)

# ── Source files ───────────────────────────────────────────────────────
set(PHOTON_SOURCES
    photon_main.c
    photon_vte.c
    photon_sdl.c
    photon_cp437_font.c
    photon_ui.c
    photon_bbslist.c
    photon_term.c
    photon_conn.c
    photon_xfer.c
    photon_store.c
    photon_settings.c
    photon_debug.c
    photon_terminus_font.c
)

if(APPLE)
    list(APPEND PHOTON_SOURCES DarwinWrappers.m)
elseif(WIN32)
    list(APPEND PHOTON_SOURCES photonterm.rc)
else()
    list(APPEND PHOTON_SOURCES early_init.c)
endif()

# ── Executable ─────────────────────────────────────────────────────────
add_executable(photonterm ${PHOTON_SOURCES})

# ── Compile definitions ────────────────────────────────────────────────
target_compile_definitions(photonterm PRIVATE
    LINK_LIST_THREADSAFE
    XPDEV_THREAD_SAFE
    _GNU_SOURCE
)

if(APPLE)
    target_compile_definitions(photonterm PRIVATE __DARWIN__ __unix__)
elseif(WIN32)
    target_compile_definitions(photonterm PRIVATE
        _WIN32
        _USE_MATH_DEFINES
        __USE_MINGW_ANSI_STDIO=1
    )
else()
    target_compile_definitions(photonterm PRIVATE __unix__)
endif()

# ── Compile options ────────────────────────────────────────────────────
target_compile_options(photonterm PRIVATE
    -Wall
    -Wno-char-subscripts
    -fno-delete-null-pointer-checks
)

if(APPLE)
    target_compile_options(photonterm PRIVATE
        -fno-common
        -mmacosx-version-min=14.0
    )
endif()

# ── Objective-C ARC (DarwinWrappers.m) ────────────────────────────────
if(APPLE)
    set_source_files_properties(DarwinWrappers.m PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# ── Include directories ────────────────────────────────────────────────
target_include_directories(photonterm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── Link libraries ─────────────────────────────────────────────────────
target_link_libraries(photonterm PRIVATE
    SDL2::SDL2-static
    SDL2_ttf::SDL2_ttf-static
    OpenSSL::SSL
    libssh2::libssh2_static
    ZLIB::ZLIB
    Threads::Threads
)

# Platform-specific link setup
if(WIN32)
    # Fully static binary - no DLL dependencies at all.
    # -static tells MinGW to link libgcc, libstdc++, and libwinpthread
    # statically instead of requiring their DLLs at runtime.
    target_link_options(photonterm PRIVATE -static -mwindows)

    # SDL2main provides WinMain -> SDL_main trampoline
    target_link_libraries(photonterm PRIVATE SDL2::SDL2main)

    # Windows system libraries needed by our deps:
    #   ws2_32     - Winsock (networking)
    #   crypt32    - OpenSSL certificate store (e_capi, winstore)
    #   gdi32      - OpenSSL / SDL2
    #   winmm      - SDL2 (multimedia timers)
    #   imm32      - SDL2 (IME input)
    #   setupapi   - SDL2 (device enumeration)
    #   version    - SDL2 (version info API)
    target_link_libraries(photonterm PRIVATE
        ws2_32 crypt32 gdi32 winmm imm32 setupapi version
    )

    # GNU ld is order-sensitive. libcrypto.a needs crypt32/gdi32 but CMake
    # places our target_link_libraries before libcrypto (a transitive dep
    # of OpenSSL::SSL via OpenSSL::Crypto). Append to the imported target's
    # interface so they appear after libcrypto.a in the link line.
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES crypt32 gdi32
    )
elseif(APPLE)
    target_link_libraries(photonterm PRIVATE
        "-framework Foundation"
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# libm - needed on POSIX; not needed on Windows (math in msvcrt)
if(NOT WIN32)
    target_link_libraries(photonterm PRIVATE m)
endif()

# ── macOS app bundle ───────────────────────────────────────────────────
if(APPLE)
    set(APP_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/PhotonTERM.app")

    # Determine optional resource paths at configure time
    set(_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/PhotonTERM.icns")
    set(_ENT  "${CMAKE_CURRENT_SOURCE_DIR}/PhotonTERM.entitlements")
    set(_FONT "${CMAKE_SOURCE_DIR}/3rdp/prefix/share/fonts/terminus-ttf/TerminusTTF.ttf")
    set(_BUNDLE_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/bundle_macos.cmake")

    # Write a cmake script that does the bundle assembly (runs at build time)
    file(WRITE "${_BUNDLE_SCRIPT}" "
set(APP_BUNDLE \"${APP_BUNDLE}\")
file(MAKE_DIRECTORY \"\${APP_BUNDLE}/Contents/MacOS\")
file(MAKE_DIRECTORY \"\${APP_BUNDLE}/Contents/Resources\")
file(COPY_FILE \"${CMAKE_CURRENT_SOURCE_DIR}/Info.plist\" \"\${APP_BUNDLE}/Contents/Info.plist\")
file(WRITE \"\${APP_BUNDLE}/Contents/PkgInfo\" \"APPL????\\n\")
if(EXISTS \"${_ICNS}\")
    file(COPY_FILE \"${_ICNS}\" \"\${APP_BUNDLE}/Contents/Resources/PhotonTERM.icns\")
endif()
if(EXISTS \"${_FONT}\")
    file(MAKE_DIRECTORY \"\${APP_BUNDLE}/Contents/Resources/Fonts\")
    file(COPY_FILE \"${_FONT}\" \"\${APP_BUNDLE}/Contents/Resources/Fonts/TerminusTTF.ttf\")
endif()
# Bundle lrzsz (sz/rz) for ZModem/YModem/XModem file transfers
set(_LSZ \"${CMAKE_SOURCE_DIR}/3rdp/prefix/bin/lsz\")
set(_LRZ \"${CMAKE_SOURCE_DIR}/3rdp/prefix/bin/lrz\")
if(EXISTS \"\${_LSZ}\")
    file(COPY_FILE \"\${_LSZ}\" \"\${APP_BUNDLE}/Contents/MacOS/sz\")
    file(COPY_FILE \"\${_LRZ}\" \"\${APP_BUNDLE}/Contents/MacOS/rz\")
endif()
")

    add_custom_command(TARGET photonterm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:photonterm>"
            "${APP_BUNDLE}/Contents/MacOS/photonterm"
        COMMAND ${CMAKE_COMMAND} -P "${_BUNDLE_SCRIPT}"
        COMMAND codesign --force --sign - --deep
            --entitlements "${_ENT}"
            "${APP_BUNDLE}" || true
        COMMENT "Assembling and signing PhotonTERM.app"
        VERBATIM
    )
endif()

# ── Install rules ──────────────────────────────────────────────────────
if(APPLE)
    install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/PhotonTERM.app"
        DESTINATION .
        USE_SOURCE_PERMISSIONS
    )
else()
    install(TARGETS photonterm
        RUNTIME DESTINATION bin
    )
endif()
