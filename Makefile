#-------------------------------------------------------------------------------
.SUFFIXES:
#-------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

#-------------------------------------------------------------------------------
APP_NAME        := Wii U Album
APP_SHORTNAME   := Wii U Album
APP_AUTHOR      := SudoTronics
#-------------------------------------------------------------------------------

include $(DEVKITPRO)/wut/share/wut_rules

TARGET          := WiiUAlbum
BUILD           := build
SOURCES         := source source/video source/album source/qr source/network source/ui
DATA            := data
INCLUDES        := source include

CFLAGS  :=      -Wall -O2 -ffunction-sections \
	                $(MACHDEP)

CFLAGS  +=      $(INCLUDE) -D__WIIU__ -D__WUT__

CXXFLAGS        := $(CFLAGS) -std=gnu++20

ASFLAGS :=      $(ARCH)
LDFLAGS  =      $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

LIBS    :=      -lSDL2_image -lwebp -ljpeg -lSDL2_ttf -lfreetype -lharfbuzz \
	        -lpng -lbz2 -lbrotlidec -lbrotlicommon -lSDL2_mixer -lvorbis -logg -lSDL2_gfx -lSDL2 -lz \
	        -Wl,--whole-archive -lavformat -Wl,--no-whole-archive -lavcodec -lswscale -lavutil -lswresample -lm -lwut

LIBDIRS := $(DEVKITPRO)/wums $(PORTLIBS) $(WUT_ROOT) $(WUT_ROOT)/usr

#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT   :=      $(CURDIR)/$(TARGET)
export TOPDIR   :=      $(CURDIR)

export VPATH    :=      $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
	                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=      $(CURDIR)/$(BUILD)

CFILES          :=      $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES        :=      $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES          :=      $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES        :=      $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD       :=      $(CC)
else
	export LD       :=      $(CXX)
endif

export OFILES_BIN       :=      $(addsuffix .o,$(BINFILES))
export OFILES_SRC       :=      $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES   :=      $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN       :=      $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  :=      $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
	                $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	                -I$(CURDIR)/$(BUILD) -I$(DEVKITPRO)/portlibs/wiiu/include/SDL2

export LIBPATHS :=      $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

WUHB_ICON       := meta/iconTex.tga
WUHB_TV_SPLASH  := meta/bootTvTex.tga
WUHB_DRC_SPLASH := meta/bootDrcTex.tga

export APP_ICON := $(TOPDIR)/$(WUHB_ICON)

ifneq (,$(wildcard $(TOPDIR)/$(WUHB_TV_SPLASH)))
export APP_TV_SPLASH := $(TOPDIR)/$(WUHB_TV_SPLASH)
endif

ifneq (,$(wildcard $(TOPDIR)/$(WUHB_DRC_SPLASH)))
export APP_DRC_SPLASH := $(TOPDIR)/$(WUHB_DRC_SPLASH)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@$(shell [ ! -d $(BUILD) ] && mkdir -p $(BUILD))
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).wuhb $(TARGET).rpx $(TARGET).elf

#-------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS :=      $(OFILES:.o=.d)

all     :       $(OUTPUT).wuhb

$(OUTPUT).wuhb  : $(OUTPUT).rpx
$(OUTPUT).rpx   :       $(OUTPUT).elf
$(OUTPUT).elf   :       $(OFILES)

$(OFILES_SRC)   : $(HFILES_BIN)

%.ttf.o %_ttf.h :       %.ttf
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
#-------------------------------------------------------------------------------
