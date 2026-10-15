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

export VPATH    := $(CURDIR)/source:$(CURDIR)/source/ui:$(CURDIR)/source/input:$(CURDIR)/source/game:$(CURDIR)/source/filesystem:$(CURDIR)/source/core:$(CURDIR)/../xenon/source:$(CURDIR)/../src
export DEPSDIR  := $(CURDIR)/$(BUILD)

# eden modules + the reused libXenon layers (gfx, input) and shared FAT32.
export OFILES   := main.o app.o ui.o nav.o library.o fs.o core.o loader.o artwork.o img.o gfx.o input.o fat32.o regskip.o

export INCLUDE  := -I$(CURDIR)/source -I$(CURDIR)/source/ui -I$(CURDIR)/source/input -I$(CURDIR)/source/game -I$(CURDIR)/source/filesystem -I$(CURDIR)/source/core -I$(CURDIR)/../src -I$(CURDIR)/../xenon/source -I$(CURDIR)/$(BUILD) -I$(LIBXENON_INC)
export LIBPATHS := -L$(LIBXENON_LIB)

export LD       := $(CXX)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@make --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).elf32

else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
$(OUTPUT).elf32: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
