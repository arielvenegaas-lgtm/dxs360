#---------------------------------------------------------------------------------
# Eden360 - Nintendo Switch game-library frontend for Xbox 360 (libXenon)
#
# Produces switch.elf32 (packaged as switch.elf). Frontend only: no emulation.
# Build inside the free60/libxenon Docker image:
#   docker run --rm -v "$PWD":/app -w /app free60/libxenon:latest bash -lc "make -C eden"
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITXENON)),)
$(error "Please set DEVKITXENON in your environment, e.g. export DEVKITXENON=/usr/local/xenon")
endif

include $(DEVKITXENON)/rules

TARGET   := switch
BUILD    := build
SOURCES  := source
INCLUDES := ../src ../xenon/source source/ui source/input source/game source/filesystem source/core

# Same -O2 -fno-tree-vectorize rule as the game build (see xenon/Makefile):
# the Xenon's VMX128 mis-executes standard AltiVec emitted at -O3.
CFLAGS   = -g -O2 -fno-tree-vectorize -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS)

LDFLAGS  = -g $(MACHDEP) -Wl,--gc-sections -Wl,-Map,$(notdir $@).map

# Skip the C++ `.eh_frame` registration at boot. libXenon's c_register_frame()
# walks the executable's `.eh_frame` before main(); on some consoles that FDE
# walk faults with a Program exception (0x700) and the UI never appears. Eden360
# does not use C++ exceptions, so the registration is unnecessary and is skipped
# by DEFAULT (see eden/source/regskip.cpp for the no-op wrapper).
#
# This patch is intentionally permanent: it is only disabled when explicitly
# asked for, via EDEN_SKIP_REGISTER_FRAME=0.
#   make -C eden EDEN_SKIP_REGISTER_FRAME=0   # stock unwinder (do not ship)
EDEN_SKIP_REGISTER_FRAME ?= 1
ifeq ($(EDEN_SKIP_REGISTER_FRAME),1)
CFLAGS   += -DEDEN_SKIP_REGISTER_FRAME
CXXFLAGS += -DEDEN_SKIP_REGISTER_FRAME
LDFLAGS  += -Wl,--wrap=__register_frame
endif

LIBS     := -lxenon -lm
LIBDIRS  :=

ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)

# Absolute path of this directory, exported so the build sub-make (whose $(CURDIR)
# is the build/ subdir) can find our linker script. See the else branch below.
export EDEN_ROOT := $(CURDIR)

# Portable emulation modules (plain C99, no libXenon dependency) live at the
# repository root. Exported so the sub-make compiles them from an absolute path.
export EMUCORE  := $(CURDIR)/../emucore

export VPATH    := $(CURDIR)/source:$(CURDIR)/source/ui:$(CURDIR)/source/input:$(CURDIR)/source/game:$(CURDIR)/source/filesystem:$(CURDIR)/source/core:$(CURDIR)/../xenon/source:$(CURDIR)/../src
export DEPSDIR  := $(CURDIR)/$(BUILD)

# eden modules + the reused libXenon layers (gfx, input) and shared FAT32, plus
# the emucore bridge (emucore.o/a64_test_entry.o) and the portable core objects
# (emu_*.o; uniquely named so they never collide with the frontend's loader.o).
export OFILES   := main.o app.o ui.o nav.o library.o fs.o core.o loader.o artwork.o img.o gfx.o input.o fat32.o regskip.o \
                   emucore.o a64_test_entry.o \
                   emu_a64.o emu_a64_mem.o emu_hle.o emu_pfs0.o emu_nso.o emu_loader.o \
                   emu_aes.o emu_keys.o emu_nca.o

export INCLUDE  := -I$(CURDIR)/source -I$(CURDIR)/source/ui -I$(CURDIR)/source/input -I$(CURDIR)/source/game -I$(CURDIR)/source/filesystem -I$(CURDIR)/source/core -I$(CURDIR)/../src -I$(CURDIR)/../xenon/source -I$(CURDIR)/$(BUILD) -I$(LIBXENON_INC) -I$(EMUCORE) -I$(EMUCORE)/a64 -I$(EMUCORE)/hle -I$(EMUCORE)/loader
export LIBPATHS := -L$(LIBXENON_LIB)

export LD       := $(CXX)

.PHONY: $(BUILD) clean a64ppc

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@make --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# Portability gate: compile the portable AArch64 interpreter and the
# deterministic self-test entry point with the same PPC/libXenon toolchain
# (not linked in here yet). Catches host-only code and byte-order assumptions.
a64ppc:
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/../emucore/a64 -c $(CURDIR)/../emucore/a64/a64.c -o /tmp/a64_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/../emucore/a64 -c $(CURDIR)/../emucore/a64/a64_mem.c -o /tmp/a64_mem_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/../emucore/a64 -I$(CURDIR)/../emucore/hle -c $(CURDIR)/../emucore/hle/hle.c -o /tmp/hle_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/../emucore/a64 -I$(CURDIR)/../emucore/loader -c $(CURDIR)/../emucore/loader/pfs0.c -o /tmp/pfs0_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/../emucore/a64 -I$(CURDIR)/../emucore/loader -c $(CURDIR)/../emucore/loader/nso.c -o /tmp/nso_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/../emucore/a64 -I$(CURDIR)/../emucore/loader -c $(CURDIR)/../emucore/loader/loader.c -o /tmp/loader_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/../emucore/crypto -c $(CURDIR)/../emucore/crypto/aes.c -o /tmp/aes_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/../emucore/loader -c $(CURDIR)/../emucore/loader/keys.c -o /tmp/keys_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/../emucore/crypto -I$(CURDIR)/../emucore/loader -c $(CURDIR)/../emucore/loader/nca.c -o /tmp/nca_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/source -c $(CURDIR)/source/a64_test_entry.c -o /tmp/a64_test_entry_ppc.o
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) -I$(CURDIR)/source -I$(CURDIR)/../emucore -I$(CURDIR)/../emucore/a64 -I$(CURDIR)/../emucore/hle -I$(CURDIR)/../emucore/loader -c $(CURDIR)/source/core/emucore.c -o /tmp/emucore_ppc.o
	@echo "a64 PPC compile OK (interpreter, guest memory, HLE bridge, PFS0/NSO/AES/keys/NCA loaders, bridge, deterministic entry point)"

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).elf32

else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

# Use our own linker script (based on libXenon's) so the emulation core's guest
# RAM lives in its own high section instead of low .bss. A big low .bss pushes
# libXenon's EHCI USB DMA structures past what the 360's controller can reach,
# which makes USB mass storage disappear; see eden/app.lds. $(EDEN_ROOT) comes
# from the parent make because $(CURDIR) here is the build/ subdirectory.
LDSCRIPT := $(EDEN_ROOT)/app.lds

# The sub-make must build the final ELF by default. Without this, the first
# explicit target below (emu_*.o) becomes the default goal and `make -C eden`
# would only compile one object instead of linking switch.elf.
.DEFAULT_GOAL := $(OUTPUT).elf32

# Portable emucore objects. Targets are relative to the build directory; the
# sources come from the exported absolute $(EMUCORE). They are compiled with the
# same flags as the rest of the frontend (libXenon PPC toolchain) but need no
# libXenon headers, so their include paths are spelled out explicitly.
EMUCORE_INC := -I$(EMUCORE) -I$(EMUCORE)/a64 -I$(EMUCORE)/hle -I$(EMUCORE)/loader -I$(EMUCORE)/crypto
emu_a64.o: $(EMUCORE)/a64/a64.c
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) $(EMUCORE_INC) -c $< -o $@
emu_a64_mem.o: $(EMUCORE)/a64/a64_mem.c
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) $(EMUCORE_INC) -c $< -o $@
emu_hle.o: $(EMUCORE)/hle/hle.c
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) $(EMUCORE_INC) -c $< -o $@
emu_pfs0.o: $(EMUCORE)/loader/pfs0.c
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) $(EMUCORE_INC) -c $< -o $@
emu_nso.o: $(EMUCORE)/loader/nso.c
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) $(EMUCORE_INC) -c $< -o $@
emu_loader.o: $(EMUCORE)/loader/loader.c
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) $(EMUCORE_INC) -c $< -o $@
emu_aes.o: $(EMUCORE)/crypto/aes.c
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) $(EMUCORE_INC) -c $< -o $@
emu_keys.o: $(EMUCORE)/loader/keys.c
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) $(EMUCORE_INC) -c $< -o $@
emu_nca.o: $(EMUCORE)/loader/nca.c
	$(CC) -std=c99 -Wall -Wextra $(CFLAGS) $(EMUCORE_INC) -c $< -o $@

#---------------------------------------------------------------------------------
$(OUTPUT).elf32: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
