#---------------------------------------------------------------------------------
# Aurora Bricks - libXenon (bare-metal Xbox 360) build
#
# Produces aurora_bricks.elf32, launchable from Aurora's File Manager or XeLL.
# Build inside the free60/libxenon Docker image:
#   docker run --rm -v "$PWD":/app -w /app free60/libxenon:latest bash -lc "make -C xenon"
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITXENON)),)
$(error "Please set DEVKITXENON in your environment, e.g. export DEVKITXENON=/usr/local/xenon")
endif

include $(DEVKITXENON)/rules

#---------------------------------------------------------------------------------
# TARGET   : output name
# SOURCES  : libXenon-specific sources (this dir)
# INCLUDES : extra headers (the shared, API-agnostic game code lives in ../src)
#---------------------------------------------------------------------------------
TARGET   := aurora_bricks
BUILD    := build
SOURCES  := source
INCLUDES := ../src

# Recursive (=) on purpose: $(INCLUDE) is only defined in the sub-make, where
# it is inherited from the exported environment of the top-level make.
#
# Keep -O2 and never use -O3 here: $(MACHDEP) adds -maltivec, and GCC's
# auto-vectorizer (turned on at -O3) emits standard AltiVec/VMX code that the
# Xenon's non-standard VMX128 ISA executes incorrectly, corrupting the
# framebuffer (the toolchain itself disables many AltiVec insns in altivec.md).
# -fno-tree-vectorize guarantees no such code is generated at any -O level.
CFLAGS   = -g -O2 -fno-tree-vectorize -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS)

LDFLAGS  = -g $(MACHDEP) -Wl,--gc-sections -Wl,-Map,$(notdir $@).map

LIBS     := -lxenon -lm
LIBDIRS  :=

ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)

export VPATH    := $(CURDIR)/source:$(CURDIR)/../src
export DEPSDIR  := $(CURDIR)/$(BUILD)

# main/gfx/input come from source/; game comes from ../src (shared, API-agnostic).
export OFILES   := main.o gfx.o input.o game.o

export INCLUDE  := -I$(CURDIR)/../src -I$(CURDIR)/$(BUILD) -I$(LIBXENON_INC)
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
