# =============================================================================
# ps4-torrent — Makefile para OpenOrbis PS4 Toolchain
#
# Pré-requisito: variável OO_PS4_TOOLCHAIN apontando para o toolchain instalado.
# Exemplo: export OO_PS4_TOOLCHAIN=/opt/openorbis
#
# Uso:
#   make          — compila e gera o .pkg
#   make clean    — limpa artefatos de build
# =============================================================================

# ---------------------------------------------------------------------------
# Verificação do toolchain
# ---------------------------------------------------------------------------
ifndef OO_PS4_TOOLCHAIN
    $(error OO_PS4_TOOLCHAIN não definido. Instale o OpenOrbis e exporte a variável.)
endif

OOSDK := $(OO_PS4_TOOLCHAIN)

# ---------------------------------------------------------------------------
# Metadados do aplicativo
# ---------------------------------------------------------------------------
TARGET    := ps4-torrent
TITLE     := PS4 Torrent
TITLE_ID  := BTRC00001

# ---------------------------------------------------------------------------
# Diretórios
# ---------------------------------------------------------------------------
PROJDIR  := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
INTDIR   := $(PROJDIR)/build
OUTDIR   := $(PROJDIR)/output
PKG_ROOT := $(INTDIR)/pkg_root

# ---------------------------------------------------------------------------
# Toolchain — LLVM/Clang cross-compiler para x86_64-freebsd (PS4)
# ---------------------------------------------------------------------------
CC           := clang
CXX          := clang++
LD           := ld.lld
CREATE_FSELF := $(OOSDK)/bin/linux/create-fself
PKG_TOOL     := $(OOSDK)/bin/linux/PkgTool.Core

# ---------------------------------------------------------------------------
# Flags de compilação
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

# Bibliotecas PS4 necessárias
LIBS := \
    SceLibcInternal \
    SceUserService  \
    ScePad          \
    SceVideoOut     \
    SceNet          \
    SceNetCtl       \
    SceSysmodule    \
    ScePosix        \
    kernel          \
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
# Fontes
# ---------------------------------------------------------------------------
SOURCES_CPP := $(shell find src -name '*.cpp')
SOURCES_C   := $(shell find src -name '*.c')

OBJECTS := \
    $(patsubst src/%.cpp, $(INTDIR)/%.o, $(SOURCES_CPP)) \
    $(patsubst src/%.c,   $(INTDIR)/%.o, $(SOURCES_C))

# ---------------------------------------------------------------------------
# Regras de build
# ---------------------------------------------------------------------------
# Gerar PKG
PKG_OUT := $(OUTDIR)/UP0000-$(TITLE_ID)_00-0000000000000000.pkg
GP4 := $(INTDIR)/project.gp4

.PHONY: all clean

all: $(PKG_OUT)

# Compilar .cpp
$(INTDIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $<

# Compilar .c
$(INTDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Linkar ELF
$(INTDIR)/$(TARGET).elf: $(OBJECTS)
	@mkdir -p $(INTDIR)
	$(LD) $(LFLAGS) $(OOSDK)/lib/crt1.o $(OOSDK)/lib/crti.o $^ $(OOSDK)/lib/crtn.o -o $@

# Gerar fake SELF (eboot.bin)
$(PKG_ROOT)/eboot.bin: $(INTDIR)/$(TARGET).elf
	@mkdir -p $(PKG_ROOT)/sce_sys
	$(CREATE_FSELF) -in $< -out $@ --eboot --paid 0x3800000000000011

# Copiar assets
$(PKG_ROOT)/sce_sys/param.sfo: assets/param.sfo
	@mkdir -p $(PKG_ROOT)/sce_sys
	@cp $< $@
	@cp assets/icon0.png $(PKG_ROOT)/sce_sys/icon0.png 2>/dev/null || true

# Gerar GP4
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
	@echo '	</files>' >> $@
	@echo '	<rootdir>' >> $@
	@echo '		<dir targ_name="sce_sys">' >> $@
	@echo '			<file targ_name="param.sfo" />' >> $@
	@echo '		</dir>' >> $@
	@echo '	</rootdir>' >> $@
	@echo '</psproject>' >> $@

$(PKG_OUT): $(GP4)
	@mkdir -p $(OUTDIR)
	LD_LIBRARY_PATH="$(HOME)/openssl1.1/lib:$$LD_LIBRARY_PATH" \
	DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
	cd $(PROJDIR) && $(PKG_TOOL) pkg_build $(GP4) $(OUTDIR)
	@echo ""
	@echo "  PKG gerado: $(PKG_OUT)"
	@echo "  Copie este arquivo para o PS4 e instale via Remote PKG Installer."

clean:
	@rm -rf $(INTDIR) $(OUTDIR)
	@echo "  Limpo."
