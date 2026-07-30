# Build snes9x (retro-go pure-C port) as a loadable ELF module for T-Deck

TOOLCHAIN ?= $(HOME)/.platformio/packages/toolchain-xtensa-esp32s3/bin
CC  = $(TOOLCHAIN)/xtensa-esp32s3-elf-gcc
SIZE = $(TOOLCHAIN)/xtensa-esp32s3-elf-size

S9X = snes9x-src/src
OUT = snes.app.elf
OBJ_DIR = obj

CFLAGS = \
	-shared -fPIC -fno-common \
	-Os -mlongcalls \
	-ffunction-sections -fdata-sections \
	-I$(S9X) \
	-DLSB_FIRST \
	-DRIGHTSHIFT_IS_SAR \
	-DPIXEL_FORMAT=RGB565 \
	-include stdio.h \
	-Wno-implicit-function-declaration \
	-Wno-unused-variable \
	-Wno-unused-but-set-variable \
	-Wno-pointer-to-int-cast \
	-Wno-int-conversion \
	-Wno-format

LDFLAGS = \
	-nostartfiles -nodefaultlibs -nostdlib \
	-lgcc \
	-Wl,-e,main \
	-Wl,--gc-sections

# All snes9x source files
S9X_C = $(wildcard $(S9X)/*.c)

# Our entry point
TDECK_C = main_tdeck.c

ALL_C = $(TDECK_C) $(S9X_C)
ALL_OBJS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(notdir $(ALL_C)))

.PHONY: all clean

all: $(OUT)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OUT): $(ALL_OBJS)
	@echo "  LINK $@"
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(SIZE) $@
	@echo "  Copying to data dir..."
	mkdir -p ../../data/lua/apps/Games/SNES
	cp $@ ../../data/lua/apps/Games/SNES/

$(OBJ_DIR)/main_tdeck.o: main_tdeck.c | $(OBJ_DIR)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: $(S9X)/%.c | $(OBJ_DIR)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJ_DIR) $(OUT)
