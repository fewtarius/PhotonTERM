# PhotonTERM

PhotonTERM is a BBS terminal client for macOS, Linux, and Windows.
It connects to bulletin board systems over Telnet and SSH with full ANSI/VT100/VT220
terminal emulation, a hardware-accelerated SDL2 display, and no external runtime
dependencies on macOS.

## Features

- **Terminal emulation** - ANSI, VT100, VT220, DEC line drawing, 256-colour and
  truecolour (24-bit RGB) SGR, OSC window title, visual BEL flash
- **Connection types** - Telnet, SSH (libssh2), local shell (PTY)
- **SSH authentication** - ssh-agent, public key (Ed25519/ECDSA/RSA), password;
  host key fingerprint stored and verified per entry
- **BBS dialing directory** - INI-based list with add/edit/delete; per-entry SSH
  credentials; persistent across sessions
- **SDL2 display** - Terminus TTF font, Unicode, hardware-accelerated rendering,
  CP437 fallback for ANSI BBS art
- **File transfer** - ZModem, YModem, XModem via bundled lrzsz
- **macOS** - signed/notarizable app bundle; no Homebrew runtime dependencies;
  static-linked against bundled OpenSSL, libssh2, SDL2, SDL2_ttf
- **Settings** - font size, terminal size, visual bell toggle; persisted to INI

## Building

### Dependencies

PhotonTERM needs SDL2, SDL2_ttf, libssh2, and OpenSSL 3.x. The included
`scripts/build-deps.sh` builds all of them from source into `3rdp/prefix/`, which
both CMake and GNUmakefile pick up automatically.

```sh
bash scripts/build-deps.sh all
```

Alternatively, install system packages (Linux/Windows only — **not for macOS distribution
builds**, which must use `build-deps.sh` to produce a statically-linked, Gatekeeper-clean app):

| Platform | Command |
|----------|---------|
| Debian/Ubuntu | `apt install libssh2-1-dev libssl-dev libsdl2-dev libsdl2-ttf-dev build-essential` |
| Fedora/RHEL | `dnf install libssh2-devel openssl-devel SDL2-devel SDL2_ttf-devel gcc make` |
| MSYS2/MinGW64 | `pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_ttf mingw-w64-x86_64-libssh2 mingw-w64-x86_64-openssl` |

### CMake (recommended)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# macOS: app bundle at
build/src/photonterm/PhotonTERM.app

# Linux/Windows: binary at
build/src/photonterm/photonterm
```

CMake finds bundled deps in `3rdp/prefix/` automatically when present.
Falls back to system packages otherwise.

### GNUmakefile

```sh
# Debug build (macOS / Linux)
make -C src/photonterm

# Release build
make -C src/photonterm RELEASE=1

# Windows (MinGW64 in MSYS2)
make -C src/photonterm win=1 RELEASE=1

# Verbose build
make -C src/photonterm QUIET=""
```

GNUmakefile output directories:
- macOS: `src/photonterm/clang.darwin.arm64.exe.debug/PhotonTERM.app`
- Linux: `src/photonterm/gcc.linux.x86_64.exe.debug/photonterm`
- Windows: `src/photonterm/mingw.win32.x86_64.exe.release/photonterm.exe`

### macOS code signing

```sh
# Ad-hoc (local use only)
codesign --force --deep --sign - \
  build/src/photonterm/PhotonTERM.app

# Developer ID (distribution)
bash scripts/sign-macos.sh build/src/photonterm/PhotonTERM.app
```

## SSH Authentication

PhotonTERM tries authentication in order:

1. **ssh-agent** via `$SSH_AUTH_SOCK` (macOS Keychain, gpg-agent, 1Password, etc.)
2. **`~/.ssh/id_ed25519`**
3. **`~/.ssh/id_ecdsa`**
4. **`~/.ssh/id_rsa`**
5. **Password** from the BBS entry or prompted at connect time

Host key fingerprints are stored per-entry and verified on subsequent connections.
Mismatches prompt the user before proceeding.

## File Transfer

ZModem, YModem, and XModem transfers are handled via lrzsz (`sz`/`rz`), built by
`scripts/build-deps.sh` and bundled into the macOS app bundle. Trigger transfers
from within a BBS session as you normally would; PhotonTERM detects the protocol
negotiation automatically.

## Shell Prompt (macOS)

PhotonTERM sets `TERM_PROGRAM=PhotonTERM` for local shell sessions.
macOS `/etc/bashrc` and `/etc/zshrc` source `/etc/bashrc_$TERM_PROGRAM`
and `/etc/zshrc_$TERM_PROGRAM` respectively. To get a colored prompt in
PhotonTERM shells, create one or both of these files:

```sh
# /etc/bashrc_PhotonTERM
export PS1='\[\033[01;32m\]\h:\[\033[01;34m\]\W \$\[\033[00m\] '

# /etc/zshrc_PhotonTERM
export PROMPT='%F{green}%m%f:%B%F{blue}%1~%f%b %f%# '
```

Alternatively, set your prompt in `~/.bashrc` or `~/.zshrc` directly -
those files are sourced regardless of `TERM_PROGRAM`.

## BBS Dialing Directory

Stored as an INI file:

| Platform | Path |
|----------|------|
| macOS | `~/Library/Preferences/PhotonTERM/PhotonTERM.lst` |
| Linux | `~/.config/PhotonTERM/PhotonTERM.lst` |
| Windows | `%APPDATA%\PhotonTERM\PhotonTERM.lst` |

## Settings

Settings are persisted separately from the BBS list:

| Platform | Path |
|----------|------|
| macOS | `~/Library/Preferences/PhotonTERM/settings.ini` |
| Linux | `~/.config/PhotonTERM/settings.ini` |
| Windows | `%APPDATA%\PhotonTERM\settings.ini` |

## CI/CD

GitHub Actions (`.github/workflows/build.yml`) builds for:

| Platform | Runner | Notes |
|----------|--------|-------|
| Linux x86_64 | `ubuntu-latest` | native GCC |
| Linux aarch64 | `ubuntu-24.04-arm` | native GCC |
| macOS arm64 | `macos-latest` | static-linked; Developer ID signed; notarized on tags |
| Windows x86_64 | `windows-latest` | MSYS2/MinGW64 |

All platforms build with `scripts/build-deps.sh` and cache dependencies via
`actions/cache` for fast incremental CI runs.

## Project Structure

```
photonterm/
├── src/photonterm/           # All PhotonTERM source
│   ├── photon_main.c         # Entry point, SDL init, main loop
│   ├── photon_vte.c/h        # ANSI/VT100/VT220 terminal emulator
│   ├── photon_sdl.c/h        # SDL2 + SDL2_ttf display and keyboard
│   ├── photon_ui.c/h         # Text-mode UI widgets (menus, lists, input)
│   ├── photon_bbslist.c/h    # BBS dialing directory UI
│   ├── photon_conn.c/h       # Network transport (Telnet, SSH, PTY)
│   ├── photon_term.c/h       # Terminal I/O loop
│   ├── photon_store.c/h      # INI-based BBS list persistence
│   ├── photon_settings.c/h   # App settings persistence
│   ├── photon_xfer.c/h       # File transfer (ZModem/YModem/XModem)
│   ├── photon_debug.c/h      # Debug log (--debug flag)
│   ├── photon_compat.h       # POSIX portability (asprintf, strlcpy, etc.)
│   ├── photon_paths.h        # Config path enum + photon_mkdir_p()
│   ├── photon_bbs.h          # BBS entry struct
│   ├── photon_cp437_font.c/h # Built-in CP437 bitmap font (fallback)
│   ├── DarwinWrappers.m/h    # macOS Objective-C (app delegate, paths)
│   ├── early_init.c          # Pre-SDL signal/env setup (Linux, constructor)
│   ├── Info.plist            # macOS app bundle metadata
│   ├── CMakeLists.txt        # CMake build rules
│   └── GNUmakefile           # GNU make build rules
├── scripts/
│   ├── build-deps.sh         # Builds OpenSSL, libssh2, SDL2, SDL2_ttf, lrzsz
│   ├── release.sh            # Version bump, commit, annotated tag
│   ├── sign-macos.sh         # macOS Developer ID signing + notarization
│   └── patches/              # Patches applied by build-deps.sh
├── CMakeLists.txt            # Root CMake project file
├── 3rdp/                     # Bundled dep build output (gitignored)
├── AGENTS.md                 # Technical reference for development
├── ICONS.md                  # Icon specification and generation guide
└── README.md                 # This file
```

## Versioning

PhotonTERM uses calendar versioning: `YYYYMMDD.N` (e.g., `20260314.1`).

```sh
./scripts/release.sh           # auto-compute next version for today
./scripts/release.sh 20260314.2  # explicit version
```

## License

PhotonTERM is distributed under the **GNU General Public License v3.0** (GPLv3).

Third-party licenses:
- **libssh2**: BSD 3-Clause
- **OpenSSL**: Apache 2.0
- **SDL2** / **SDL2_ttf**: zlib License
- **Terminus TTF**: SIL Open Font License 1.1
- **lrzsz**: GNU GPL v2+
