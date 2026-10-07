# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS +=

# --- Ableton Link bridge -------------------------------------------------------
# Reuse WAIL's shared Link C wrapper instead of re-vendoring the SDK. It lives
# one level up in plugins/, and #includes vendor/link's abl_link.cpp (the whole
# header-only Ableton Link 4.0 + Link Audio SDK), so we compile ONE extra TU.
# VPATH lets us list it by basename so its object lands in build/ (not ../).
# Mirrors the include paths + C++17 in plugins/CMakeLists.txt (add_wail_link_plugin).
VPATH += ../plugins
SOURCES += wail_link.cpp

FLAGS += -I../plugins
FLAGS += -I../vendor/link/extensions/abl_link/include
FLAGS += -I../vendor/link/extensions/abl_link/src
FLAGS += -I../vendor/link/include
FLAGS += -I../vendor/link/modules/asio-standalone/asio/include
# -------------------------------------------------------------------------------

# --- Embedded WAIL core (Go, -buildmode=c-archive) ----------------------------
# wail-app/cmd/wailcore built as a static library and linked into plugin.dylib,
# so the plugin stays one binary. GOOS/GOARCH follow Rack's ARCH_OS/ARCH_CPU;
# CC is passed through so a cross toolchain builds the cgo parts too.
WAILCORE := build/libwailcore.a
GO ?= go
GOOS_mac := darwin
GOOS_lin := linux
GOOS_win := windows
GOARCH_x64 := amd64
GOARCH_arm64 := arm64
WAILCORE_SRC := $(shell find ../wail-app/cmd/wailcore -name '*.go' ! -name '*_test.go')

$(WAILCORE): $(WAILCORE_SRC)
	@mkdir -p build
	cd ../wail-app && CGO_ENABLED=1 GOOS=$(GOOS_$(ARCH_OS)) GOARCH=$(GOARCH_$(ARCH_CPU)) CC="$(CC)" \
		$(GO) build -buildmode=c-archive -trimpath -ldflags "-s -w -X main.version=$$(cat ../VERSION)" \
		-o ../rack/$(WAILCORE) ./cmd/wailcore

OBJECTS += $(WAILCORE)
build/src/WailCore.cpp.o: $(WAILCORE)
# (Its platform link flags follow plugin.mk, which defines ARCH_OS.)
# -------------------------------------------------------------------------------


# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

# --- Link build settings that depend on the framework --------------------------
# Appended AFTER plugin.mk so (a) ARCH_OS/ARCH_CPU from arch.mk are defined and
# (b) this -std lands last (compile.mk sets -std=c++11, then folds FLAGS in),
# so C++17 wins for the Link TU. C++-only via CXXFLAGS (the plugin has no .c).
CXXFLAGS += -std=c++17

ifeq ($(ARCH_OS), mac)
	FLAGS += -DLINK_PLATFORM_MACOSX=1 -DLINK_PLATFORM_UNIX=1
endif
ifeq ($(ARCH_OS), lin)
	# _GNU_SOURCE: wail_link.h's lb_module_stamp needs dladdr (glibc hides it
	# otherwise); -ldl for glibc < 2.34. Mirrors plugins/CMakeLists.txt.
	FLAGS += -DLINK_PLATFORM_LINUX=1 -DLINK_PLATFORM_UNIX=1 -D_GNU_SOURCE
	LDFLAGS += -latomic -ldl
endif
# The Go runtime's system dependencies (wailcore).
ifeq ($(ARCH_OS), mac)
	LDFLAGS += -framework CoreFoundation -framework Security
endif
ifeq ($(ARCH_OS), lin)
	LDFLAGS += -lpthread
endif
ifeq ($(ARCH_OS), win)
	# NOTE: MinGW also needs the vendored Channels.hpp `interface` param rename
	# (see WAIL DEVELOPMENT.md / release.yml). Windows is untested for this plugin.
	FLAGS += -DLINK_PLATFORM_WINDOWS=1 -DNOMINMAX
	LDFLAGS += -lws2_32 -lwinmm -liphlpapi
endif
# -------------------------------------------------------------------------------

# --- Unit tests for the pure timing logic (host compiler, no Rack/Link) -------
.PHONY: test
test: build/test_main
	./build/test_main

build/test_main: test/test_main.cpp src/FrameClock.hpp src/StampAligner.hpp src/RoomSlots.hpp
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -Wall -Wextra -o $@ test/test_main.cpp
# -------------------------------------------------------------------------------
