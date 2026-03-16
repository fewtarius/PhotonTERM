# PhotonTERM - standalone build
#
# Requires: 3rdp/prefix built by scripts/build-deps.sh all
#   or system SDL2, SDL2_ttf, libssh2, OpenSSL 3.x
#
# Usage:
#   make WITHOUT_JPEG_XL=1                    # debug build (macOS/Linux)
#   make WITHOUT_JPEG_XL=1 RELEASE=1          # optimised release build
#   make WITHOUT_JPEG_XL=1 win=1              # cross-compile MinGW64

# ── Toolchain defaults ─────────────────────────────────────────────────
CC      ?= cc
CXX     ?= c++
AR      ?= ar
INSTALL ?= install

# Detect OS / arch
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
  os := darwin
else ifeq ($(UNAME_S),Linux)
  os := linux
else
  os := unknown
endif
ifdef win
  os := win
  CC  := x86_64-w64-mingw32-gcc
  CXX := x86_64-w64-mingw32-g++
endif

# ── Build type ──────────────────────────────────────────────────────────
ifdef RELEASE
  BUILDPATH := release
  OPT       := -O3
else
  BUILDPATH := debug
  OPT       := -ggdb -D_DEBUG
endif

# Output dirs
ifeq ($(os),darwin)
  ARCH    := arm64
  CC_PFX  := clang
else ifeq ($(os),win)
  ARCH    := x86_64
  CC_PFX  := gcc
else
  ARCH    := $(UNAME_M)
  CC_PFX  := gcc
endif
OBJDIR  := $(CC_PFX).$(os).$(ARCH).exe.$(BUILDPATH)
$(shell mkdir -p $(OBJDIR))

# ── Third-party prefix ─────────────────────────────────────────────────
DEPS_PREFIX := $(abspath ../../3rdp/prefix)
ifeq ($(wildcard $(DEPS_PREFIX)/lib/libssh2.a),$(DEPS_PREFIX)/lib/libssh2.a)
  USE_BUNDLED_DEPS := 1
endif

# ── SDL2 ───────────────────────────────────────────────────────────────
ifdef USE_BUNDLED_DEPS
  SDL_CONFIG     := $(DEPS_PREFIX)/bin/sdl2-config
  SDL2_CFLAGS    := $(shell $(SDL_CONFIG) --cflags)
  SDL2_LDFLAGS   := $(shell $(SDL_CONFIG) --static-libs)
  TTF_CFLAGS     :=
  TTF_LDFLAGS    := $(DEPS_PREFIX)/lib/libSDL2_ttf.a \
                    $(DEPS_PREFIX)/lib/libfreetype.a
else
  SDL2_CFLAGS    := $(shell sdl2-config --cflags 2>/dev/null || pkg-config sdl2 --cflags 2>/dev/null)
  SDL2_LDFLAGS   := $(shell sdl2-config --libs   2>/dev/null || pkg-config sdl2 --libs   2>/dev/null)
  TTF_CFLAGS     :=
  TTF_LDFLAGS    := -lSDL2_ttf -lfreetype
endif

# ── libssh2 + OpenSSL ─────────────────────────────────────────────────
ifdef USE_BUNDLED_DEPS
  OPENSSL_LIBDIR := $(or \
    $(and $(wildcard $(DEPS_PREFIX)/lib/libssl.a),$(DEPS_PREFIX)/lib),\
    $(and $(wildcard $(DEPS_PREFIX)/lib64/libssl.a),$(DEPS_PREFIX)/lib64))
  SSH2_CFLAGS    := -I$(DEPS_PREFIX)/include
  SSH2_LDFLAGS   := $(DEPS_PREFIX)/lib/libssh2.a \
                    $(OPENSSL_LIBDIR)/libssl.a \
                    $(OPENSSL_LIBDIR)/libcrypto.a \
                    -lz
  ifeq ($(os),darwin)
    SSH2_LDFLAGS += -framework Security -framework CoreFoundation
  endif
  ifdef win
    SSH2_LDFLAGS += -lcrypt32 -lgdi32 -lws2_32
  endif
else ifeq ($(os),darwin)
  SSH2_CFLAGS  :=
  SSH2_LDFLAGS := $(shell brew --prefix libssh2 2>/dev/null)/lib/libssh2.a \
                  $(shell brew --prefix openssl@3 2>/dev/null)/lib/libssl.a \
                  $(shell brew --prefix openssl@3 2>/dev/null)/lib/libcrypto.a \
                  -lz
else
  SSH2_CFLAGS  := $(shell pkg-config --cflags libssh2 2>/dev/null)
  SSH2_LDFLAGS := $(shell pkg-config --libs   libssh2 2>/dev/null || echo -lssh2) -lssl -lcrypto -lz
endif

# ── Compile flags ──────────────────────────────────────────────────────
CFLAGS := $(OPT) -Wall -Wno-char-subscripts -fno-delete-null-pointer-checks \
           $(SDL2_CFLAGS) $(TTF_CFLAGS) $(SSH2_CFLAGS) \
           -DLINK_LIST_THREADSAFE -DXPDEV_THREAD_SAFE

ifeq ($(os),darwin)
  CFLAGS += -D__DARWIN__ -D__unix__ -fno-common \
             -mmacosx-version-min=14.0
else ifeq ($(os),linux)
  CFLAGS += -D__unix__
else ifdef win
  CFLAGS += -D_WIN32 -D_USE_MATH_DEFINES
endif

# SDL TTF header may be in SDL2/ subdir or directly in include
ifdef USE_BUNDLED_DEPS
  CFLAGS += -I$(DEPS_PREFIX)/include
endif

LDFLAGS :=
ifeq ($(os),darwin)
  LDFLAGS += -framework Foundation
endif
ifdef win
  # Fully static: no DLL deps (libgcc, libstdc++, libwinpthread all static)
  LDFLAGS += -static -mwindows
endif

# ── Source objects ─────────────────────────────────────────────────────
OBJ := $(OBJDIR)/photon_main.o \
       $(OBJDIR)/photon_vte.o \
       $(OBJDIR)/photon_sdl.o \
       $(OBJDIR)/photon_cp437_font.o \
       $(OBJDIR)/photon_ui.o \
       $(OBJDIR)/photon_bbslist.o \
       $(OBJDIR)/photon_term.o \
       $(OBJDIR)/photon_conn.o \
       $(OBJDIR)/photon_xfer.o \
       $(OBJDIR)/photon_store.o \
       $(OBJDIR)/photon_settings.o \
       $(OBJDIR)/photon_debug.o \
       $(OBJDIR)/photon_terminus_font.o

ifeq ($(os),darwin)
  OBJ += $(OBJDIR)/DarwinWrappers.o
else ifndef win
  OBJ += $(OBJDIR)/early_init.o
endif

# ── Target ─────────────────────────────────────────────────────────────
ifeq ($(os),darwin)
  BINARY := $(OBJDIR)/photonterm
  APPBUNDLE := $(OBJDIR)/PhotonTERM.app
else ifdef win
  BINARY := $(OBJDIR)/photonterm.exe
else
  BINARY := $(OBJDIR)/photonterm
endif

all: $(BINARY)
ifdef ifeq($(os),darwin)
all: $(APPBUNDLE)
endif

$(BINARY): $(OBJ)
	@echo "Linking $@"
	$(CXX) $(LDFLAGS) $(OBJ) \
	    $(SDL2_LDFLAGS) $(TTF_LDFLAGS) $(SSH2_LDFLAGS) \
	    -lpthread -lm \
	    -o $@
ifeq ($(os),darwin)
	$(MAKE) bundle BINARY=$(BINARY)
endif

# ── macOS app bundle ───────────────────────────────────────────────────
APPBUNDLE := $(OBJDIR)/PhotonTERM.app
bundle: $(BINARY)
	mkdir -p $(APPBUNDLE)/Contents/MacOS
	mkdir -p $(APPBUNDLE)/Contents/Resources
	cp $(BINARY) $(APPBUNDLE)/Contents/MacOS/photonterm
	cp Info.plist $(APPBUNDLE)/Contents/
	-cp PhotonTERM.icns $(APPBUNDLE)/Contents/Resources/ 2>/dev/null || true
	echo "APPL????" > $(APPBUNDLE)/Contents/PkgInfo
	# Bundle sz/rz (lrzsz) for ZModem/XModem/YModem file transfers
	$(if $(wildcard $(DEPS_PREFIX)/bin/lsz), \
	    cp $(DEPS_PREFIX)/bin/lsz $(APPBUNDLE)/Contents/MacOS/sz && \
	    cp $(DEPS_PREFIX)/bin/lrz $(APPBUNDLE)/Contents/MacOS/rz, \
	    true)
	-codesign --force --sign - --entitlements PhotonTERM.entitlements \
	    --deep $(APPBUNDLE) 2>/dev/null || true

# ── Compile rules ──────────────────────────────────────────────────────
$(OBJDIR)/%.o: %.c
	@echo "CC $<"
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/DarwinWrappers.o: DarwinWrappers.m
	@echo "ObjC DarwinWrappers.m"
	$(CC) $(CFLAGS) -fobjc-arc -MMD -MP -c -o $@ $<

-include $(wildcard $(OBJDIR)/*.d)

# ── Clean ───────────────────────────────────────────────────────────────
clean:
	rm -rf $(OBJDIR)

.PHONY: all bundle clean
