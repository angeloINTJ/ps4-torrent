# =============================================================================
# ps4-torrent — Makefile for the OpenOrbis PS4 Toolchain
#
# Prerequisite: OO_PS4_TOOLCHAIN variable pointing to the installed toolchain.
# Example: export OO_PS4_TOOLCHAIN=/opt/openorbis/OpenOrbis/PS4Toolchain
#
# Usage:
#   make          — build and generate the .pkg
#   make clean    — remove build artifacts
# =============================================================================

# ---------------------------------------------------------------------------
# Toolchain check (with automatic fallback detection)
# ---------------------------------------------------------------------------
ifndef OO_PS4_TOOLCHAIN
    ifneq ($(wildcard /opt/openorbis/OpenOrbis/PS4Toolchain),)
        OO_PS4_TOOLCHAIN := /opt/openorbis/OpenOrbis/PS4Toolchain
    else ifneq ($(wildcard $(HOME)/openorbis/OpenOrbis/PS4Toolchain),)
        OO_PS4_TOOLCHAIN := $(HOME)/openorbis/OpenOrbis/PS4Toolchain
    else
        $(error OO_PS4_TOOLCHAIN not set. Install OpenOrbis and export the variable.)
    endif
endif

export OO_PS4_TOOLCHAIN
OOSDK := $(OO_PS4_TOOLCHAIN)

# ---------------------------------------------------------------------------
# Application metadata
# ---------------------------------------------------------------------------
TARGET    := ps4-torrent
TITLE     := PS4 Torrent
TITLE_ID  := BTRC00001

# ---------------------------------------------------------------------------
# Directories
# ---------------------------------------------------------------------------
PROJDIR  := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
INTDIR   := $(PROJDIR)/build
OUTDIR   := $(PROJDIR)/output
PKG_ROOT := $(INTDIR)/pkg_root

# ---------------------------------------------------------------------------
# Toolchain — LLVM/Clang cross-compiler for x86_64-freebsd (PS4)
# ---------------------------------------------------------------------------
export PATH := $(HOME)/bin:$(PATH)
export LD_LIBRARY_PATH := $(HOME)/openssl1.1/lib:$(LD_LIBRARY_PATH)
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1
export DOTNET_BUNDLE_EXTRACT_BASE_DIR := $(HOME)/.cache/dotnet_bundle_extract

CC           := clang
CXX          := clang++
LD           := ld.lld
CREATE_FSELF := $(OOSDK)/bin/linux/create-fself
PKG_TOOL     := $(OOSDK)/bin/linux/PkgTool.Core

# ---------------------------------------------------------------------------
# Compilation flags
# ---------------------------------------------------------------------------
TARGET_TRIPLE := x86_64-pc-freebsd12-elf

CFLAGS := \
    -target $(TARGET_TRIPLE) \
    -fPIC                    \
    -funwind-tables          \
    -O2                      \
    -Wall                    \
    -Wextra                  \
    -c                       \
    -D_GNU_SOURCE            \
    -D_BSD_SOURCE            \
    -I$(OOSDK)/include       \
    -I$(OOSDK)/include/c++/v1 \
    -Iinclude

CXXFLAGS := $(CFLAGS) -std=c++17

# Required PS4 libraries
LIBS := \
    SceLibcInternal  \
    SceSystemService \
    SceUserService   \
    ScePad           \
    SceVideoOut      \
    SceNet           \
    SceNetCtl        \
    SceSysmodule     \
    ScePosix         \
    kernel           \
    kernel_sys

LFLAGS := \
    -m elf_x86_64            \
    -pie                     \
    --script $(OOSDK)/link.x \
    --eh-frame-hdr           \
    -L$(OOSDK)/lib           \
    --start-group            \
    $(addprefix -l, $(LIBS)) \
    -lc++                    \
    -lc++abi                 \
    -lc++experimental        \
    -lm                      \
    -lc                      \
    --end-group              \
    --undefined=__inet_aton

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
SOURCES_CPP := $(shell find src -name '*.cpp')
SOURCES_C   := $(shell find src -name '*.c')

OBJECTS := \
    $(patsubst src/%.cpp, $(INTDIR)/%.o, $(SOURCES_CPP)) \
    $(patsubst src/%.c,   $(INTDIR)/%.o, $(SOURCES_C))

# ---------------------------------------------------------------------------
# Build rules
# ---------------------------------------------------------------------------
PKG_OUT := $(OUTDIR)/UP0000-$(TITLE_ID)_00-0000000000000000.pkg
GP4     := $(INTDIR)/project.gp4

.PHONY: all clean test

all: $(PKG_OUT)

# Run automated unit + end-to-end TCP integration test suite on host
test:
	@mkdir -p $(INTDIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude \
		tests/e2e_test.cpp \
		src/bencode.cpp \
		src/metainfo.cpp \
		src/peer_wire.cpp \
		src/piece_manager.cpp \
		src/session.cpp \
		src/tracker.cpp \
		-o $(INTDIR)/e2e_test
	$(INTDIR)/e2e_test

# Compile .cpp
$(INTDIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $<

# Compile .c
$(INTDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Link ELF
$(INTDIR)/$(TARGET).elf: $(OBJECTS)
	@mkdir -p $(INTDIR)
	$(LD) $(LFLAGS) $(OOSDK)/lib/crt1.o $(OOSDK)/lib/crti.o $^ $(OOSDK)/lib/crtn.o -o $@

# Generate Fake SELF (eboot.bin)
# Note: -out specifies the intermediate Orbis ELF (.oelf), while --eboot=<path>
# produces the signed Fake SELF (magic 4F 15 3D 1D) required by PS4 GoldHEN.
$(PKG_ROOT)/eboot.bin: $(INTDIR)/$(TARGET).elf
	@mkdir -p $(PKG_ROOT)/sce_sys
	$(CREATE_FSELF) -in=$< -out=$(INTDIR)/$(TARGET).oelf --eboot=$@ --paid 0x3800000000000011

# Copy assets (param.sfo, icon0.png, right.sprx)
$(PKG_ROOT)/sce_sys/param.sfo: assets/param.sfo
	@mkdir -p $(PKG_ROOT)/sce_sys/about
	@cp $< $@
	@cp assets/icon0.png $(PKG_ROOT)/sce_sys/icon0.png 2>/dev/null || true
	@cp $(OOSDK)/src/modules/right.sprx $(PKG_ROOT)/sce_sys/about/right.sprx 2>/dev/null || true

# Generate GP4
$(GP4): $(PKG_ROOT)/eboot.bin $(PKG_ROOT)/sce_sys/param.sfo
	@echo '<?xml version="1.0"?>' > $@
	@echo '<psproject fmt="gp4" version="1000">' >> $@
	@echo '	<volume>' >> $@
	@echo '		<volume_type>pkg_ps4_app</volume_type>' >> $@
	@echo '		<volume_id>PS4VOLUME</volume_id>' >> $@
	@echo '		<volume_ts>'$$(date '+%Y-%m-%d %H:%M:%S')'</volume_ts>' >> $@
	@echo '		<package content_id="UP0000-$(TITLE_ID)_00-0000000000000000"' >> $@
	@echo '		         passcode="00000000000000000000000000000000"' >> $@
	@echo '		         storage_type="digital50"' >> $@
	@echo '		         app_type="full" />' >> $@
	@echo '		<chunk_info chunk_count="1" scenario_count="1">' >> $@
	@echo '			<chunks>' >> $@
	@echo '				<chunk id="0" layer_no="0" label="Chunk #0" />' >> $@
	@echo '			</chunks>' >> $@
	@echo '			<scenarios default_id="0">' >> $@
	@echo '				<scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>' >> $@
	@echo '			</scenarios>' >> $@
	@echo '		</chunk_info>' >> $@
	@echo '	</volume>' >> $@
	@echo '	<files img_no="0">' >> $@
	@echo '		<file targ_path="eboot.bin" orig_path="pkg_root/eboot.bin" />' >> $@
	@echo '		<file targ_path="sce_sys/param.sfo" orig_path="pkg_root/sce_sys/param.sfo" />' >> $@
	@if [ -f "$(PKG_ROOT)/sce_sys/icon0.png" ]; then \
		echo '		<file targ_path="sce_sys/icon0.png" orig_path="pkg_root/sce_sys/icon0.png" />' >> $@; \
	fi
	@if [ -f "$(PKG_ROOT)/sce_sys/about/right.sprx" ]; then \
		echo '		<file targ_path="sce_sys/about/right.sprx" orig_path="pkg_root/sce_sys/about/right.sprx" />' >> $@; \
	fi
	@echo '	</files>' >> $@
	@echo '	<rootdir>' >> $@
	@echo '		<dir targ_name="sce_sys">' >> $@
	@echo '			<dir targ_name="about" />' >> $@
	@echo '		</dir>' >> $@
	@echo '	</rootdir>' >> $@
	@echo '</psproject>' >> $@

$(PKG_OUT): $(GP4)
	@mkdir -p $(OUTDIR) $(DOTNET_BUNDLE_EXTRACT_BASE_DIR)
	cd $(PROJDIR) && $(PKG_TOOL) pkg_build $(GP4) $(OUTDIR)
	@cp $(INTDIR)/$(TARGET).elf $(OUTDIR)/$(TARGET).elf
	@cp $(PKG_ROOT)/eboot.bin $(OUTDIR)/eboot.bin
	@echo ""
	@echo "  PKG generated: $(PKG_OUT)"
	@echo "  ELF generated: $(OUTDIR)/$(TARGET).elf"
	@echo "  Copy the PKG to the PS4 and install via GoldHEN / Remote PKG Installer,"
	@echo "  or send $(OUTDIR)/$(TARGET).elf via an ELF Loader on FW <= 14.02."

clean:
	@rm -rf $(INTDIR) $(OUTDIR)
	@echo "  Clean."
