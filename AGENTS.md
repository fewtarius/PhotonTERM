# AGENTS.md

**Version:** 3.0
**Date:** 2026-03-14
**Purpose:** Technical reference for PhotonTERM development

---

## Project Overview

PhotonTERM is a C-language BBS terminal client for macOS, Linux, and Windows.
It uses SDL2 + SDL2_ttf for display, libssh2 + OpenSSL for SSH, and lrzsz for
file transfer. The entire codebase is original. All modules are named `photon_*`.

---

## Quick Setup

```bash
# Build bundled deps (recommended - OpenSSL, libssh2, SDL2, SDL2_ttf, lrzsz)
bash scripts/build-deps.sh all

# CMake build (preferred)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc || sysctl -n hw.logicalcpu)

# GNUmakefile build (macOS / Linux)
make -C src/photonterm

# Run (macOS)
open build/src/photonterm/PhotonTERM.app
# or
open src/photonterm/clang.darwin.arm64.exe.debug/PhotonTERM.app

# Run with debug log
build/src/photonterm/photonterm --debug
# Log written to /tmp/photonterm-PID.log
```

---

## Architecture

```
User -> photon_bbslist   (BBS directory UI)
            |
        photon_conn      (transport: Telnet / SSH via libssh2 / PTY)
            |
        photon_term      (terminal I/O loop)
            |
        photon_vte       (ANSI/VT100/VT220 emulator)
            |
        photon_sdl       (SDL2 + SDL2_ttf display)
            |
        photon_ui        (text UI widgets: menus, lists, input boxes)
```

**Threading model:**
- Main thread: SDL event loop + UI
- Per-connection: rx thread (network -> ring buffer -> VTE) and tx thread
  (ring buffer -> network); heap-allocated I/O buffers (RING_SIZE = 256 KB)
- Ring buffers are mutex-protected; `ring_wait_bytes()` for backpressure

---

## Directory Structure

| Path | Purpose |
|------|---------|
| `src/photonterm/photon_main.c` | Entry point: SDL init, main loop, settings load |
| `src/photonterm/photon_vte.c/h` | ANSI/VT100/VT220/VT52 terminal emulator |
| `src/photonterm/photon_sdl.c/h` | SDL2 + SDL2_ttf display, keyboard, BEL flash |
| `src/photonterm/photon_ui.c/h` | Text UI widgets: menus, input, message boxes |
| `src/photonterm/photon_bbslist.c/h` | BBS dialing directory UI |
| `src/photonterm/photon_conn.c/h` | Transport: Telnet, SSH (libssh2), PTY |
| `src/photonterm/photon_term.c/h` | Terminal I/O session loop |
| `src/photonterm/photon_store.c/h` | INI-based BBS list persistence |
| `src/photonterm/photon_settings.c/h` | App settings persistence |
| `src/photonterm/photon_xfer.c/h` | File transfer (ZModem/YModem/XModem via lrzsz) |
| `src/photonterm/photon_debug.c/h` | Debug log infrastructure (--debug flag) |
| `src/photonterm/photon_compat.h` | POSIX portability (asprintf, strlcpy, etc.) |
| `src/photonterm/photon_paths.h` | Config path enum + photon_mkdir_p() |
| `src/photonterm/photon_bbs.h` | photon_bbs_t struct (BBS entry definition) |
| `src/photonterm/photon_cp437_font.c/h` | Built-in CP437 bitmap font data |
| `src/photonterm/DarwinWrappers.m/h` | macOS Objective-C wrappers |
| `src/photonterm/early_init.c` | Pre-SDL signal/env setup (Linux, constructor attr) |
| `src/photonterm/Info.plist` | macOS app bundle metadata |
| `src/photonterm/CMakeLists.txt` | CMake build target |
| `src/photonterm/GNUmakefile` | GNU make build rules |
| `CMakeLists.txt` | Root CMake project (delegates to src/photonterm/) |
| `scripts/build-deps.sh` | Builds OpenSSL, libssh2, SDL2, SDL2_ttf, lrzsz |
| `scripts/release.sh` | Version bump, commit, annotated tag |
| `scripts/sign-macos.sh` | macOS Developer ID signing + notarization |
| `scripts/patches/` | Patches applied by build-deps.sh (e.g., lrzsz) |
| `.github/workflows/build.yml` | CI: Linux x86_64/aarch64, macOS arm64, Windows |
| `AGENTS.md` | This file |
| `README.md` | User-facing documentation |
| `ICONS.md` | Icon specification |

---

## Code Style

**C Conventions:**

- Tabs for indentation (4-space display width)
- `//` line comments in new code; `/* */` block comments for file/section headers
- `strlcpy` / `strlcat` for safe string copy (via `photon_compat.h`)
- `asprintf` for dynamic string formatting (alloc + sprintf)
- Error handling: return values; `photon_ui_msg()` for user-visible errors
- Function names: `lowercase_with_underscores`
- Macros: `UPPERCASE_WITH_UNDERSCORES`
- `static` for file-scope functions

**Comments:**
- Brief, what not why - git history handles why
- No dramatic annotations (`CRITICAL FIX`, `HACK`, etc.)
- No AI-generated patterns (`ensure that`, `this handles the case where`)

**Build system:**
- CMake is the primary build system (preferred for new work and CI)
- GNUmakefile is retained for fast iterative local development
- Output dirs (GNUmakefile): `clang.darwin.arm64.exe.debug/` on macOS,
  `gcc.linux.x86_64.exe.debug/` on Linux
- `QUIET = @` suppresses command echo; `make QUIET=""` shows full commands

---

## Build Systems

### CMake

Root `CMakeLists.txt` delegates to `src/photonterm/CMakeLists.txt`.

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release   # or Debug

# Build
cmake --build build -j$(nproc)

# Install (Linux: /usr/local/bin; macOS: ./PhotonTERM.app)
cmake --install build --prefix /usr/local
```

CMake finds dependencies via `CMAKE_PREFIX_PATH`. When `3rdp/prefix/` exists
(built by `build-deps.sh`), it is prepended automatically. Otherwise falls back
to system packages found by `find_package`.

Key CMake variables:
| Variable | Effect |
|----------|--------|
| `CMAKE_BUILD_TYPE` | `Debug` (default) or `Release` |
| `CMAKE_PREFIX_PATH` | Override dep search path |
| `USE_BUNDLED_DEPS` | Auto-set to ON if `3rdp/prefix/lib/libssh2.a` exists |

### GNUmakefile

```bash
make -C src/photonterm               # debug (default)
make -C src/photonterm RELEASE=1     # release (-O3)
make -C src/photonterm win=1         # Windows cross-compile
make -C src/photonterm QUIET=""      # verbose
make -C src/photonterm clean         # clean output dir
```

---

## Module Naming Conventions

- All source files: `photon_<name>.c` / `photon_<name>.h`
- Test drivers: `photon_<name>_test.c` (standalone, not in main binary)
- Settings/paths: `PHOTON_PATH_*` enum in `photon_paths.h`
- BBS entry fields: `photon_bbs_t` in `photon_bbs.h`

---

## VTE (Terminal Emulator)

`src/photonterm/photon_vte.c` - the core emulator.

**Supported sequences:**
- SGR: standard 16-color, 256-color (`38;5;N`), truecolor (`38;2;R;G;B`)
- Cursor: CUP, CUU/CUD/CUF/CUB, DECSC/DECRC (ESC 7/8 and CSI s/u)
- Erase: ED, EL (all variants)
- DEC: DECSTBM (scroll region), DECOM (origin mode), DECTCEM (cursor visibility)
- Character sets: G0/G1 via ESC ( / ESC ) + SO/SI; DEC line drawing (`ESC(0`)
- OSC: OSC 0 and OSC 2 set window title via `title` callback
- BEL: `0x07` fires `bell` callback
- CP437 mode: toggled per-connection based on protocol (Telnet BBS = CP437)

**Callbacks** (set in `photon_vte_callbacks_t`):
- `draw_cell` - render a character cell
- `set_cursor` - move/show/hide cursor
- `scroll` - scroll a region up/down
- `resize` - terminal grid changed
- `title` - window title update (OSC 0/2)
- `bell` - BEL character received

---

## SSH Implementation

`src/photonterm/photon_conn.c` - SSH transport via libssh2.

**Authentication order:**
1. ssh-agent (`$SSH_AUTH_SOCK`)
2. `~/.ssh/id_ed25519`
3. `~/.ssh/id_ecdsa`
4. `~/.ssh/id_rsa`
5. Password (from BBS entry or prompted)

**Host key verification:**
- First connect: auto-accept, store fingerprint in BBS entry
- Subsequent connects: compare stored vs received; prompt on mismatch

**I/O threads:**
- All four I/O threads (telnet rx/tx, ssh rx/tx) use heap-allocated buffers
  (`malloc(RING_SIZE)`) to avoid stack overflow on platforms with small thread
  stacks (macOS default: 512 KB; RING_SIZE = 256 KB)

---

## Settings

`src/photonterm/photon_settings.c` - persisted to `settings.ini`.

Current keys:
| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `font_size` | int | 16 | TTF font point size |
| `term_cols` | int | 80 | Terminal grid columns |
| `term_rows` | int | 25 | Terminal grid rows |
| `bell` | bool | true | Visual bell enabled |

---

## Testing

**Before committing:**

```bash
# Clean rebuild (CMake)
cmake --build build --clean-first

# Clean rebuild (GNUmakefile)
make -C src/photonterm clean && make -C src/photonterm

# Run the app
open src/photonterm/clang.darwin.arm64.exe.debug/PhotonTERM.app

# Verify no external dylib dependencies (macOS)
otool -L src/photonterm/clang.darwin.arm64.exe.debug/photonterm \
  | grep -v "/System/Library\|/usr/lib"
# Should output nothing

# Check binary architecture
file src/photonterm/clang.darwin.arm64.exe.debug/photonterm
```

**Manual test checklist:**
- [ ] Telnet connection works
- [ ] SSH connection with agent works
- [ ] SSH connection with key file works
- [ ] Window close exits app
- [ ] SSH host fingerprint prompted on first connect, stored, verified on reconnect
- [ ] BBS directory: add, edit, delete entries
- [ ] Settings menu: font size, terminal size, visual bell toggle all persist
- [ ] BEL flash fires on `^G` from shell
- [ ] OSC title updates window titlebar
- [ ] Truecolor sequences render correct RGB cells

---

## Commit Format

```
type(scope): brief description

Longer explanation if needed.
```

**Types:** `feat`, `fix`, `refactor`, `chore`, `docs`, `build`, `ci`
**Scopes:** `ssh`, `ui`, `vte`, `sdl`, `conn`, `store`, `settings`, `xfer`,
            `makefile`, `cmake`, `build`, `ci`, `docs`

**Examples:**
```
fix(conn): heap-allocate I/O thread buffers to prevent stack overflow
feat(vte): OSC title, truecolor rendering, BEL flash with settings toggle
build: add root CMakeLists.txt, update .gitignore
```

Git workflow: squash commits before pushing. Do not push to origin without asking.

---

## Version and Release

The version string lives in `photon_main.c`:

```c
const char *photonterm_version = "PhotonTERM YYYYMMDD.N";
```

Use `scripts/release.sh` to bump the version, update `Info.plist`, commit, and tag.

```bash
./scripts/release.sh             # auto-compute next version for today
./scripts/release.sh 20260314.1  # explicit version
# Does NOT push - push separately after review
git push origin main --tags
```

CI `verify-version` job blocks tag builds if the source version doesn't match the tag.

---

## Common Patterns

**Adding debug logging to a source file:**
```c
#define PHOTON_DEBUG_BUILD
#include "photon_debug.h"

PHOTON_DBG("function: var=%d ptr=%p", val, (void *)ptr);
```
Output only appears when launched with `--debug`; goes to `/tmp/photonterm-PID.log`.

**Getting a config path:**
```c
char path[1024];
photon_config_path(PHOTON_PATH_CONFIG, path, sizeof(path));
```

**Showing a UI error message:**
```c
photon_ui_msg(ui, "Connection failed: %s", strerror(errno));
```

---

## macOS-Specific Notes

**App bundle structure:**
```
PhotonTERM.app/
└── Contents/
    ├── Info.plist
    ├── MacOS/
    │   ├── photonterm       # main binary
    │   ├── sz               # lrzsz sender (ZModem/YModem/XModem)
    │   └── rz               # lrzsz receiver
    └── Resources/
        ├── PhotonTERM.icns
        └── Fonts/
            └── TerminusTTF.ttf
```

**LDFLAGS order matters:** SDL2/SSH/SSL flags come after object files in link line.

**Shell environment:** PhotonTERM sets `TERM_PROGRAM=PhotonTERM` for local shell
sessions.  macOS `/etc/bashrc` and `/etc/zshrc` source
`/etc/{bash,zsh}rc_$TERM_PROGRAM`.  Users wanting a colored prompt when launching
from Finder/Spotlight should create `/etc/bashrc_PhotonTERM` and/or
`/etc/zshrc_PhotonTERM` (see README for examples), or set their prompt in
`~/.bashrc` / `~/.zshrc` directly.

**Code signing:**
```bash
# Ad-hoc (local use only)
codesign --force --deep --sign - PhotonTERM.app

# Developer ID (distribution)
./scripts/sign-macos.sh PhotonTERM.app
```

---

## CI/CD

`.github/workflows/build.yml` - four matrix jobs:

| Job | Platform | Runner |
|-----|----------|--------|
| `build-linux-x86_64` | Linux x86_64 | `ubuntu-latest` |
| `build-linux-aarch64` | Linux aarch64 | `ubuntu-24.04-arm` |
| `build-macos-arm64` | macOS arm64 | `macos-latest` |
| `build-windows-x86_64` | Windows x86_64 | `windows-latest` (MSYS2) |

- All platforms cache `3rdp/prefix/` via `actions/cache` keyed on dep versions
- macOS signs on every push to `main`; notarizes only on version tags
- `verify-version` job gates tag builds to ensure source version matches tag

Apple CI secret names: `APPLE_CERTIFICATE_P12`, `APPLE_CERTIFICATE_PASSWORD`,
`APPLE_DEVELOPER_ID`, `APPLE_ID`, `APPLE_APP_PASSWORD`, `APPLE_TEAM_ID`

---

## Anti-Patterns

| Anti-Pattern | Why Wrong | What To Do |
|--------------|-----------|------------|
| `strcpy` / `sprintf` | Buffer overflow risk | `strlcpy`, `snprintf`, `asprintf` |
| Stack buffers >= RING_SIZE | Stack overflow on small stacks | `malloc`/`free` |
| Hardcoded paths (`/opt/homebrew/...`) | Machine-specific | Use `pkg-config` or CMake `find_package` |
| Editing photon_sdl.c for VTE logic | Wrong layer | Keep layers separated |
| Pushing to origin without asking | Repo policy | Squash, present for review first |
| Dynamic linking Homebrew libs on macOS | Breaks on other machines | Static link `.a` from `3rdp/prefix` |

---

## Quick Reference

```bash
# Build
cmake -B build && cmake --build build -j$(nproc)
# or
make -C src/photonterm -j$(nproc || sysctl -n hw.logicalcpu)

# Run (macOS)
open src/photonterm/clang.darwin.arm64.exe.debug/PhotonTERM.app

# Run with debug log
src/photonterm/clang.darwin.arm64.exe.debug/photonterm --debug

# Check deps
otool -L src/photonterm/clang.darwin.arm64.exe.debug/photonterm | grep -v "System\|/usr/lib"

# Sign for local use
codesign --force --deep --sign - \
  src/photonterm/clang.darwin.arm64.exe.debug/PhotonTERM.app

# Release
./scripts/release.sh
git push origin main --tags
```

---

## License

PhotonTERM is distributed under the **GNU General Public License v3.0** (GPLv3).

Third-party licenses:
- **libssh2**: BSD 3-Clause
- **OpenSSL**: Apache 2.0
- **SDL2** / **SDL2_ttf**: zlib License
- **Terminus TTF**: SIL Open Font License 1.1
- **lrzsz**: GNU GPL v2+
