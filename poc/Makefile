#---------------------------------------------------------------------------------
# HK sprite PoC - libXenon (bare-metal Xbox 360) build
#
# Produces hk_sprite_poc.elf32: it loads ONE .spr off a FAT32 USB stick at
# runtime and displays it. No game asset is ever compiled into the binary.
# Build inside the free60/libxenon Docker image:
#   docker run --rm -v "$PWD":/app -w /app free60/libxenon:latest bash -lc "make -C poc"
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITXENON)),)
$(error "Please set DEVKITXENON in your environment, e.g. export DEVKITXENON=/usr/local/xenon")
endif

include $(DEVKITXENON)/rules

#---------------------------------------------------------------------------------
# Everything is reused from the game: the Xenon gfx+input, and the shared .spr
# decoder + FAT32 reader. Only main.cpp is PoC-specific.
#---------------------------------------------------------------------------------
TARGET   := hk_sprite_poc
BUILD    := build
SOURCES  := source
INCLUDES := ../src ../xenon/source

# Same -O2 -fno-tree-vectorize rule as xenon/Makefile (see that file for why).
CFLAGS   = -g -O2 -fno-tree-vectorize -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS)

LDFLAGS  = -g $(MACHDEP) -Wl,--gc-sections -Wl,-Map,$(notdir $@).map

LIBS     := -lxenon -lm
LIBDIRS  :=

ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)

export VPATH    := $(CURDIR)/source:$(CURDIR)/../xenon/source:$(CURDIR)/../src
export DEPSDIR  := $(CURDIR)/$(BUILD)

# main from source/; gfx+input+fat32 from ../xenon/source; img from ../src.
export OFILES   := main.o gfx.o input.o fat32.o img.o

export INCLUDE  := -I$(CURDIR)/../src -I$(CURDIR)/../xenon/source -I$(CURDIR)/$(BUILD) -I$(LIBXENON_INC)
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
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).elf32: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
