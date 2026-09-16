# Makefile for ukncbtl-debugger
#
# Mirrors the Debug/Release configurations of ukncbtldebug.vcxproj (VS2022).
#
# Usage:
#   make            # Release build (default)
#   make debug      # Debug build (-D_DEBUG, asserts enabled, no optimization)
#   make release    # Release build explicitly
#   make clean       # Remove build artifacts for both configs
#   make run         # Build (release) and run
#   make run-debug   # Build (debug) and run
#
# Binaries are placed in build/<config>/ukncbtldebug

CXX      := g++
CXXSTD   := -std=c++17
TARGET   := ukncbtldebug

SRCS := \
	ukncbtldebug.cpp \
	Common.cpp \
	Emulator.cpp \
	stdafx.cpp \
	emubase/Board.cpp \
	emubase/Disasm.cpp \
	emubase/Floppy.cpp \
	emubase/Hard.cpp \
	emubase/Memory.cpp \
	emubase/Processor.cpp \
	emubase/SoundAY.cpp \
	util/GdbServer.cpp \
	util/Screen.cpp

INCLUDES := -I.

WARNINGS := -Wall -Wextra -Wno-unused-parameter

# --- SDL3, if there is any --------------------------------------------------
#
# Only --screen needs it (see util/Screen.cpp), and the way this is
# usually run -- by uknc-run, or by an editor -- has no screen at all,
# so a build without SDL3 is a build, not a failure: Screen.cpp keeps
# its functions and they do nothing.
#
# The .pc file is found through Homebrew's prefix as well as whatever
# pkg-config already looks at: a pkg-config that came from somewhere
# else (MacPorts', for one) searches its own tree and not Homebrew's,
# and then a perfectly installed SDL3 looks missing.
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
# On the command itself rather than exported: the make that comes with
# macOS is 3.81, and an exported variable does not reach $(shell).
PKG_CONFIG := PKG_CONFIG_PATH="$(PKG_CONFIG_PATH):$(BREW_PREFIX)/lib/pkgconfig" pkg-config

SDL3_LIBS := $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)
ifneq ($(SDL3_LIBS),)
SDL3_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null) -DHAVE_SDL3
endif

# --- Configuration-specific flags -------------------------------------------

RELEASE_DIR   := build/release
DEBUG_DIR     := build/debug

RELEASE_FLAGS := -O2 -DNDEBUG
DEBUG_FLAGS   := -O0 -g -D_DEBUG

# --- Default target -----------------------------------------------------

.PHONY: all release debug clean run run-debug

all: release

release: $(RELEASE_DIR)/$(TARGET)

debug: $(DEBUG_DIR)/$(TARGET)

# --- Object file lists ----------------------------------------------------

RELEASE_OBJS := $(patsubst %.cpp,$(RELEASE_DIR)/%.o,$(SRCS))
DEBUG_OBJS   := $(patsubst %.cpp,$(DEBUG_DIR)/%.o,$(SRCS))

# --- Link -------------------------------------------------------------------

$(RELEASE_DIR)/$(TARGET): $(RELEASE_OBJS)
	$(CXX) $(RELEASE_OBJS) $(SDL3_LIBS) -o $@

$(DEBUG_DIR)/$(TARGET): $(DEBUG_OBJS)
	$(CXX) $(DEBUG_OBJS) $(SDL3_LIBS) -o $@

# --- Compile ------------------------------------------------------------

$(RELEASE_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARNINGS) $(INCLUDES) $(SDL3_CFLAGS) $(RELEASE_FLAGS) -c $< -o $@

$(DEBUG_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARNINGS) $(INCLUDES) $(SDL3_CFLAGS) $(DEBUG_FLAGS) -c $< -o $@

# --- Convenience targets --------------------------------------------------

run: release
	./$(RELEASE_DIR)/$(TARGET)

run-debug: debug
	./$(DEBUG_DIR)/$(TARGET)

clean:
	rm -rf build
