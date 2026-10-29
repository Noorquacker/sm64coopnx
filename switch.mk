# --------------------------------------
# Nintendo Switch (libnx/devkitA64) build
# --------------------------------------
# Included by the Makefile when TARGET_SWITCH=1. Everything Switch-specific
# lives here so upstream Makefile changes merge cleanly. Variables that the
# Makefile assigns later are pinned with 'override'.
#
#   make TARGET_SWITCH=1                      -> build/us_nx/sm64coopnx.nro
#   make TARGET_SWITCH=1 nsp KEYS=prod.keys   -> build/us_nx/sm64coopnx.nsp

ifeq ($(strip $(DEVKITPRO)),)
  $(error DEVKITPRO is not set. Install devkitPro and export DEVKITPRO=/opt/devkitpro)
endif

LIBNX    := $(DEVKITPRO)/libnx
PORTLIBS := $(DEVKITPRO)/portlibs/switch
DKP_TOOLS := $(DEVKITPRO)/tools/bin

VERSION ?= us

SWITCH_TITLE    ?= sm64coopnx
SWITCH_AUTHOR   ?= coop-deluxe
SWITCH_VERSION  ?= $(shell awk '/define VERSION_NUMBER/{a=$$3} /define MINOR_VERSION_NUMBER/{b=$$3} END{print "v" a "." b}' src/pc/network/version.h)
# Arbitrary homebrew application ID used for the NSP (low 12 bits must be zero)
SWITCH_TITLE_ID ?= 0100C0DE5A640000
SWITCH_ICON     ?= res/switch/icon.jpg
SWITCH_NPDM     ?= res/switch/sm64coopnx.json

# Features with no Switch implementation (see docs/switch-port.md)
override DISCORD_SDK   := 0
override UPDATER       := 0
override TARGET_RPI    := 0
override TARGET_RK3588 := 0
override OSX_BUILD     := 0
override WINDOWS_BUILD := 0
override TARGET_BITS   := 0

# CoopNet (public lobbies): lib/coopnet/switch, built by tools/switch/build_coopnet.sh
ifeq ($(COOPNET),1)
  SWITCH_COOPNET_LIBS := -Llib/coopnet/switch -l:libcoopnet.a -l:libjuice.a
endif

# SDL window + OpenGL backend: replaced by the deko3d backend (src/pc/switch/gfx_*)
SWITCH_EXCLUDED_C_FILES := src/pc/gfx/gfx_opengl.c src/pc/gfx/gfx_sdl.c
# dual-stack IPv6 sockets: the Switch only has IPv4 (src/pc/switch/socket_switch.c)
SWITCH_EXCLUDED_C_FILES += src/pc/network/socket/socket.c src/pc/network/socket/socket_linux.c

override BUILD_DIR := build/$(VERSION)_nx
override EXE       := $(BUILD_DIR)/sm64coopnx.elf
SWITCH_NACP  := $(BUILD_DIR)/sm64coopnx.nacp
SWITCH_ROMFS := $(BUILD_DIR)/romfs
SWITCH_NRO   := $(BUILD_DIR)/sm64coopnx.nro
SWITCH_NSP   := $(BUILD_DIR)/sm64coopnx.nsp

# Toolchain
override CROSS     := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
override SDLCONFIG := $(PORTLIBS)/bin/sdl2-config

SWITCH_ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

EXTRA_CFLAGS += $(SWITCH_ARCH) -ffunction-sections -D__SWITCH__ -DTARGET_SWITCH \
                -isystem $(LIBNX)/include -I$(PORTLIBS)/include -Ilib/uam/include

# In C++, gcc's <limits.h> leaves LLONG_MAX to <climits>, which luaconf.h needs
EXTRA_CPP_FLAGS += -include climits

# DEKO3D_DEBUG=1 links the validating deko3d library, which reports API misuse
DEKO3D_DEBUG ?= 0
ifeq ($(DEKO3D_DEBUG),1)
  DEKO3D_LIB := -ldeko3dd
else
  DEKO3D_LIB := -ldeko3d
endif

override LDFLAGS := -specs=$(LIBNX)/switch.specs $(SWITCH_ARCH) -Wl,-Map,$(BUILD_DIR)/sm64coopnx.map \
                    -Llib/lua/switch -l:liblua53.a -Llib/uam/switch -luam $(SWITCH_COOPNET_LIBS) \
                    -L$(PORTLIBS)/lib -lcurl -lz $(shell $(SDLCONFIG) --static-libs) \
                    -L$(LIBNX)/lib $(DEKO3D_LIB) -lnx -lm

# --------------------------------------
# Packaging
# --------------------------------------

all: $(SWITCH_NRO)

$(SWITCH_NACP):
	@$(PRINT) "$(GREEN)Creating NACP: $(BLUE)$@ $(NO_COL)\n"
	$(V)$(DKP_TOOLS)/nacptool --create "$(SWITCH_TITLE)" "$(SWITCH_AUTHOR)" "$(SWITCH_VERSION)" $@ --titleid=$(SWITCH_TITLE_ID)

# Built-in resources ship read-only inside the NRO/NSP romfs
switch-romfs: $(EXE)
	@$(PRINT) "$(GREEN)Staging romfs: $(BLUE)$(SWITCH_ROMFS) $(NO_COL)\n"
	$(V)rm -rf $(SWITCH_ROMFS)
	$(V)mkdir -p $(SWITCH_ROMFS)
	$(V)cp -r $(BUILD_DIR)/lang $(BUILD_DIR)/mods $(BUILD_DIR)/palettes $(BUILD_DIR)/dynos $(SWITCH_ROMFS)/
	$(V)cp -r res/switch/licenses $(SWITCH_ROMFS)/

$(SWITCH_NRO): $(EXE) $(SWITCH_NACP) $(SWITCH_ICON) switch-romfs
	@$(PRINT) "$(GREEN)Building NRO: $(BLUE)$@ $(NO_COL)\n"
	$(V)$(DKP_TOOLS)/elf2nro $(EXE) $@ --icon=$(SWITCH_ICON) --nacp=$(SWITCH_NACP) --romfsdir=$(SWITCH_ROMFS)

# The NSP is signed with the user's own console keys; keys are never stored in the repo
nsp: $(SWITCH_NSP)

$(SWITCH_NSP): $(EXE) $(SWITCH_NACP) $(SWITCH_ICON) $(SWITCH_NPDM) switch-romfs
ifeq ($(strip $(KEYS)),)
	$(error Building an NSP requires your console keys: make TARGET_SWITCH=1 nsp KEYS=/path/to/prod.keys)
endif
	@$(PRINT) "$(GREEN)Building NSP: $(BLUE)$@ $(NO_COL)\n"
	$(V)rm -rf $(BUILD_DIR)/nsp
	$(V)mkdir -p $(BUILD_DIR)/nsp/exefs $(BUILD_DIR)/nsp/control
	$(V)$(DKP_TOOLS)/elf2nso $(EXE) $(BUILD_DIR)/nsp/exefs/main
	$(V)$(DKP_TOOLS)/npdmtool $(SWITCH_NPDM) $(BUILD_DIR)/nsp/exefs/main.npdm
	$(V)cp $(SWITCH_NACP) $(BUILD_DIR)/nsp/control/control.nacp
	$(V)cp $(SWITCH_ICON) $(BUILD_DIR)/nsp/control/icon_AmericanEnglish.dat
	$(V)hacbrewpack -k $(KEYS) --titleid $(SWITCH_TITLE_ID) --nologo \
		--exefsdir $(BUILD_DIR)/nsp/exefs --romfsdir $(SWITCH_ROMFS) --controldir $(BUILD_DIR)/nsp/control \
		--tempdir $(BUILD_DIR)/nsp/temp --backupdir $(BUILD_DIR)/nsp/backup --ncadir $(BUILD_DIR)/nsp/nca \
		--nspdir $(BUILD_DIR)/nsp/out
	$(V)mv $(BUILD_DIR)/nsp/out/*.nsp $@

# Convenience: launch the NRO in yuzu. Put your ROM in <yuzu sdmc>/switch/sm64coopnx/ first.
YUZU ?= yuzu
run-yuzu: $(SWITCH_NRO)
	$(YUZU) $(SWITCH_NRO)

.PHONY: switch-romfs nsp run-yuzu
