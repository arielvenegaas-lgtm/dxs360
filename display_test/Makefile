#---------------------------------------------------------------------------------
# display_test - minimal standalone Xenos / display bring-up test (libXenon)
#
# Produces display_test.elf32: a KNOWN colour-bar pattern with a moving sweep
# marker and a live counter, built on the repository's real Gfx_* API
# (src/gfx.cpp). No assets, no game code, no USB.
#
# Build inside the free60/libxenon Docker image:
#   docker run --rm -v "$PWD":/app -w /app free60/libxenon:latest bash -lc "make -C display_test"
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITXENON)),)
$(error "Please set DEVKITXENON in your environment, e.g. export DEVKITXENON=/usr/local/xenon")
endif

include $(DEVKITXENON)/rules

TARGET   := display_test
BUILD    := build
SOURCES  := source
INCLUDES := ../src

# Same -O2 -fno-tree-vectorize rule as the other builds (see xenon/Makefile):
# the Xenon's VMX128 core mis-executes standard AltiVec emitted at -O3.
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

# main from source/; gfx (the shared framebuffer path) from ../src.
export OFILES   := main.o gfx.o

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

$(OUTPUT).elf32: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
