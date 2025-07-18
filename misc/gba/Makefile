ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro)
endif

.SUFFIXES:


#---------------------------------------------------------------------------------
# Configurable options
#---------------------------------------------------------------------------------
# Name of the final output
TARGET 		= ClassiCube0-gba
# List of directories containing source code
SOURCE_DIRS	= src src/gba
# Directory where object files are placed
BUILD_DIR	= build/gba

GAME_TITLE	= ClassiCube0
GAME_CODE	= 0000
MAKER_CODE	= 00


#---------------------------------------------------------------------------------
# Compilable files
#---------------------------------------------------------------------------------
S_FILES = $(foreach dir,$(SOURCE_DIRS),$(wildcard $(dir)/*.S))
C_FILES = $(foreach dir,$(SOURCE_DIRS),$(wildcard $(dir)/*.c))
OBJS 	= $(addprefix $(BUILD_DIR)/, $(notdir $(C_FILES:%.c=%.o) $(S_FILES:%.S=%.o)))

# Dependency tracking
DEPFLAGS = -MT $@ -MMD -MP -MF $(BUILD_DIR)/$*.d
DEPFILES := $(OBJS:%.o=%.d)


#---------------------------------------------------------------------------------
# Code generation
#---------------------------------------------------------------------------------
ARCH	= -mthumb -mthumb-interwork
CFLAGS	= -g -Wall -O2 -DPLAT_GBA -ffunction-sections -fdata-sections -mcpu=arm7tdmi -mtune=arm7tdmi $(ARCH)
ASFLAGS	= -g $(ARCH)

LDFLAGS	= -specs=gba.specs -g $(ARCH)
LIBS	= -ltonc

LIBGBA		:= $(DEVKITPRO)/libgba
INCLUDES 	+= $(foreach dir, $(LIBGBA), -I$(dir)/include)
LDFLAGS 	+= $(foreach dir, $(LIBGBA), -L$(dir)/lib)

LIBTONC		= $(DEVKITPRO)/libtonc
INCLUDES 	+= $(foreach dir, $(LIBTONC), -I$(dir)/include)
LDFLAGS 	+= $(foreach dir, $(LIBTONC), -L$(dir)/lib)


#---------------------------------------------------------------------------------
# Compiler tools
#---------------------------------------------------------------------------------
PREFIX	= $(DEVKITPRO)/devkitARM/bin/arm-none-eabi-
ARM_AS	= $(PREFIX)as
ARM_CC	= $(PREFIX)gcc
ARM_OBJCOPY = $(PREFIX)objcopy

GBAFIX = $(DEVKITPRO)/tools/bin/gbafix


#---------------------------------------------------------------------------------
# Main targets
#---------------------------------------------------------------------------------
default: $(BUILD_DIR) $(TARGET).gba

clean:
	rm $(TARGET).gba $(TARGET).elf $(OBJS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)


#---------------------------------------------------------------------------------
# Executable generation
#---------------------------------------------------------------------------------
$(TARGET).elf: $(OBJS)
	$(ARM_CC) $(LDFLAGS) $^ -o $@ $(LIBS)

$(TARGET).gba: $(TARGET).elf
	$(ARM_OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -t$(GAME_TITLE) -c$(GAME_CODE) -m$(MAKER_CODE)


#---------------------------------------------------------------------------------
# Object generation
#---------------------------------------------------------------------------------
$(BUILD_DIR)/%.o: src/%.c
	$(ARM_CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/gba/%.c
	$(ARM_CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -c $< -o $@


#---------------------------------------------------------------------------------
# Dependency tracking
#---------------------------------------------------------------------------------
$(DEPFILES):

include $(wildcard $(DEPFILES))
