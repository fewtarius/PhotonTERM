# PhotonTERM - top-level GNUmakefile
# Delegates to src/photonterm/GNUmakefile

SHELL := /bin/bash
JOBS  ?= $(shell sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)

.PHONY: all deps build clean distclean install icons release sign notarize \
        openssl libssh2 sdl2 deps-clean

# --- Dependency build -------------------------------------------------------

# Build all third-party deps (libssh2, OpenSSL, SDL2) into 3rdp/prefix/
deps:
	@echo "Building third-party dependencies..."
	JOBS=$(JOBS) bash scripts/build-deps.sh all

# Individual dep targets
openssl:
	JOBS=$(JOBS) bash scripts/build-deps.sh openssl

libssh2: openssl
	JOBS=$(JOBS) bash scripts/build-deps.sh libssh2

sdl2:
	JOBS=$(JOBS) bash scripts/build-deps.sh sdl2

# Remove built deps (keep downloads)
deps-clean:
	bash scripts/build-deps.sh clean

# Remove built deps AND downloads
distclean: clean
	bash scripts/build-deps.sh distclean

# --- Main build ------------------------------------------------------------

# Build the application (uses bundled deps if available)
all build:
	$(MAKE) -C src/photonterm -j$(JOBS)

# Build everything from scratch: deps then app
from-scratch: deps all

# Clean app build artifacts (not deps)
clean:
	$(MAKE) -C src/photonterm clean

# --- Asset generation -------------------------------------------------------

# Regenerate all platform icons from SVG masters
# Requires: librsvg (rsvg-convert) + imagemagick
icons:
	cd src/photonterm/icons && bash generate-icons.sh all

# --- Release builds ---------------------------------------------------------

# Release build (optimized, stripped)
release:
	$(MAKE) -C src/photonterm RELEASE=1 -j$(JOBS)

# Full release from scratch: deps + release build
release-from-scratch: deps release

# --- macOS signing ----------------------------------------------------------
# Requires: APPLE_DEVELOPER_ID env var set (for full signing)
# Without APPLE_DEVELOPER_ID: ad-hoc sign only

APP := $(firstword $(wildcard src/photonterm/*.exe.*/PhotonTERM.app \
                  src/photonterm/clang.darwin.arm64.exe.debug/PhotonTERM.app \
                  src/photonterm/clang.darwin.arm64.exe.release/PhotonTERM.app))

# Ad-hoc sign (local use, no Apple Developer account needed)
sign-adhoc:
	@if [ -z "$(APP)" ]; then echo "ERROR: No app bundle found. Run 'make all' first."; exit 1; fi
	codesign --force --deep --sign - "$(APP)"
	@echo "Ad-hoc signed: $(APP)"

# Full sign + notarize (requires Apple Developer ID)
sign: release
	@if [ -z "$$APPLE_DEVELOPER_ID" ] && [ ! -f scripts/sign-macos.sh ]; then \
		echo "ERROR: APPLE_DEVELOPER_ID not set"; exit 1; fi
	bash scripts/sign-macos.sh

# Notarize a pre-signed app or zip
notarize:
	bash scripts/sign-macos.sh notarize-only

# --- Install ----------------------------------------------------------------

install:
	$(MAKE) -C src/photonterm install

# --- Info -------------------------------------------------------------------

info:
	@echo "PhotonTERM build system"
	@echo ""
	@echo "Targets:"
	@echo "  deps            Build libssh2, OpenSSL, SDL2 into 3rdp/prefix/"
	@echo "  all             Build PhotonTERM (uses bundled deps if available)"
	@echo "  from-scratch    deps + all"
	@echo "  release         Optimized build"
	@echo "  release-from-scratch  deps + release"
	@echo "  sign-adhoc      Ad-hoc code sign (local use)"
	@echo "  sign            Full sign + notarize (requires Apple Developer ID)"
	@echo "  icons           Regenerate icons from SVG"
	@echo "  clean           Remove app build artifacts"
	@echo "  deps-clean      Remove built deps (keep downloads)"
	@echo "  distclean       Remove everything"
	@echo ""
	@echo "Variables:"
	@echo "  JOBS=$(JOBS)"
	@if [ -d 3rdp/prefix/lib ]; then \
		echo "  Bundled deps: YES (3rdp/prefix/)"; \
		ls 3rdp/prefix/lib/*.a 2>/dev/null | xargs -I{} basename {} | tr '\n' ' '; \
		echo ""; \
	else \
		echo "  Bundled deps: NO (run 'make deps' to build)"; \
	fi
