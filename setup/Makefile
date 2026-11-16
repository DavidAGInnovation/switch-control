ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must be set)
endif
include $(DEVKITPRO)/libnx/switch_rules

TARGET := switch-control-setup
BUILD := build
APP_TITLE := Switch Control Setup
APP_AUTHOR := Switch tools
APP_VERSION := 0.5.0
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -O2 -Wall -Wextra -Werror -ffunction-sections $(ARCH) -I$(LIBNX)/include
LDFLAGS := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(TARGET).map
LIBS := -lnx
export APP_TITLE APP_AUTHOR APP_VERSION

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH := $(CURDIR)/source
export DEPSDIR := $(CURDIR)/$(BUILD)
export OFILES := main.o
export LD := $(CC)
export LIBPATHS := -L$(LIBNX)/lib
export NROFLAGS := --nacp=$(OUTPUT).nacp
export CFLAGS LDFLAGS LIBS
.PHONY: all clean
all:
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	rm -rf $(BUILD) $(TARGET).nro $(TARGET).elf $(TARGET).nacp
else
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(OFILES:.o=.d)
endif
