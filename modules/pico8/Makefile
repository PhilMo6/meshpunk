# Build fake-08 as a loadable ELF module for ESP32-S3 T-Deck
# Uses the Xtensa toolchain from PlatformIO

TOOLCHAIN ?= $(HOME)/.platformio/packages/toolchain-xtensa-esp32s3/bin
CC  = $(TOOLCHAIN)/xtensa-esp32s3-elf-gcc
CXX = $(TOOLCHAIN)/xtensa-esp32s3-elf-g++
SIZE = $(TOOLCHAIN)/xtensa-esp32s3-elf-size

F08 = fake08-src
OUT = pico8.app.elf
OBJ_DIR = obj

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
# -fwrapv / -fno-strict-aliasing are correctness flags, not tuning: z8lua
# emulates PICO-8's 16.16 fixed-point with int32 arithmetic whose wraparound
# is the spec (signed overflow is UB the optimizer exploits at -O2), and it's
# C type-punning code compiled as C++.
COMMON_FLAGS = \
	-shared -fPIC -fno-common \
	-Os -mlongcalls \
	-fwrapv -fno-strict-aliasing \
	-ffunction-sections -fdata-sections \
	-D_TDECK \
	-I$(F08)/source \
	-I$(F08)/libs/z8lua \
	-I$(F08)/libs/lodepng \
	-I$(F08)/libs/utf8-util \
	-I$(F08)/libs/simpleini \
	-I$(F08)/libs/miniz

CFLAGS = $(COMMON_FLAGS) \
	-Wno-implicit-function-declaration

CXXFLAGS = $(COMMON_FLAGS) \
	-fno-rtti -fno-exceptions \
	-std=gnu++17 \
	-Wno-write-strings

LDFLAGS = \
	-nostartfiles -nodefaultlibs -nostdlib \
	-lgcc \
	-L$(HOME)/.platformio/packages/toolchain-xtensa-esp32s3/xtensa-esp32s3-elf/lib/no-rtti \
	-lstdc++ \
	-lgcc \
	-Wl,-e,main \
	-Wl,--gc-sections

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------

# Our platform files
TDECK_CPP = \
	main_tdeck.cpp \
	TDeckHost.cpp \
	cxxstubs.cpp \
	filehelpers_tdeck.cpp

# fake-08 core source — exclude files we replace with T-Deck implementations
F08_EXCLUDE = $(F08)/source/main.cpp $(F08)/source/hostCommonFunctions.cpp $(F08)/source/filehelpers.cpp
F08_CPP = $(filter-out $(F08_EXCLUDE), $(wildcard $(F08)/source/*.cpp))

# z8lua (C files)
Z8LUA_C = $(wildcard $(F08)/libs/z8lua/*.c)

# lodepng
LODEPNG_CPP = $(F08)/libs/lodepng/lodepng.cpp

# All sources
CPP_SRCS = $(TDECK_CPP) $(F08_CPP) $(LODEPNG_CPP)
C_SRCS   = $(Z8LUA_C)

# Object files (flatten into obj/)
CPP_OBJS = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(notdir $(CPP_SRCS)))
C_OBJS   = $(patsubst %.c,   $(OBJ_DIR)/%.o, $(notdir $(C_SRCS)))
ALL_OBJS = $(CPP_OBJS) $(C_OBJS)

# ---------------------------------------------------------------------------
# Build rules
# ---------------------------------------------------------------------------

.PHONY: all clean

all: $(OUT)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Link
$(OUT): $(ALL_OBJS)
	@echo "  LINK $@"
	$(CXX) $(COMMON_FLAGS) $(LDFLAGS) -o $@ $^
	$(SIZE) $@
	@echo "  Copying to data dir..."
	mkdir -p ../../data/lua/apps/Games/PICO-8
	cp $@ ../../data/lua/apps/Games/PICO-8/

# ---------------------------------------------------------------------------
# Pattern rules — compile each source to obj/basename.o
# We need vpath or explicit rules since sources are in different dirs.
# ---------------------------------------------------------------------------

# C++ files from our platform dir
$(OBJ_DIR)/main_tdeck.o: main_tdeck.cpp | $(OBJ_DIR)
	@echo "  CXX main_tdeck.cpp"
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJ_DIR)/TDeckHost.o: TDeckHost.cpp | $(OBJ_DIR)
	@echo "  CXX TDeckHost.cpp"
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJ_DIR)/cxxstubs.o: cxxstubs.cpp | $(OBJ_DIR)
	@echo "  CXX cxxstubs.cpp"
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJ_DIR)/filehelpers_tdeck.o: filehelpers_tdeck.cpp | $(OBJ_DIR)
	@echo "  CXX filehelpers_tdeck.cpp"
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Hot-path files compiled -O2 instead of -Os: the audio synth runs per sample
# and the PICO-8 gfx API per draw call. (gcc honors the last -O flag.)
$(OBJ_DIR)/Audio.o $(OBJ_DIR)/synth.o $(OBJ_DIR)/filter.o $(OBJ_DIR)/graphics.o: HOT_OPT = -O2

# C++ files from fake08-src/source/
$(OBJ_DIR)/%.o: $(F08)/source/%.cpp | $(OBJ_DIR)
	@echo "  CXX $<"
	$(CXX) $(CXXFLAGS) $(HOT_OPT) -c -o $@ $<

# lodepng
$(OBJ_DIR)/lodepng.o: $(F08)/libs/lodepng/lodepng.cpp | $(OBJ_DIR)
	@echo "  CXX lodepng.cpp"
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# z8lua .c files — compiled as C++ because luaconf.h includes <cstdint>.
# Force setjmp/longjmp for error handling (C++ exceptions don't work in PIC ELF modules).
# z8lua runs -O2: the interpreter dominates cart step time. The historical
# -O2 corruption was UB (fix32 signed overflow + type punning), pinned down
# by -fwrapv -fno-strict-aliasing in CXXFLAGS — those are the precondition
# for -O2 here, never remove them.
$(OBJ_DIR)/%.o: $(F08)/libs/z8lua/%.c | $(OBJ_DIR)
	@echo "  CXX $< (z8lua -O2)"
	$(CXX) $(CXXFLAGS) -O2 -DLUA_USE_LONGJMP -x c++ -c -o $@ $<

clean:
	rm -rf $(OBJ_DIR) $(OUT)
