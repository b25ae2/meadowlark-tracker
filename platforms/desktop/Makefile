# Meadowlark desktop build (phase 0 spike).
# Builds core/ for the host as Tracker Mini without the proprietary synth,
# plus the desktop platform layer. A CMake build replaces this in phase 0 proper.
#
#   make -C platforms/desktop -j      -> platforms/desktop/build/meadowlark

ROOT_DIR    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CORE_DIR    := $(ROOT_DIR)/core
UTIL_DIR    := $(ROOT_DIR)/util
SYNTH_DIR   := $(ROOT_DIR)/synth
DESKTOP_DIR := $(ROOT_DIR)/platforms/desktop
BUILD_DIR   := $(DESKTOP_DIR)/build
OBJ_DIR     := $(BUILD_DIR)/obj

CXX ?= c++
CC  ?= cc

include $(ROOT_DIR)/version.mk
PRODUCT := MINI

CORE_SRCS := $(shell cd $(CORE_DIR) && find source libs interface effector FileManager Exporter AudioEngine synth \( -name '*.cpp' -o -name '*.c' \) | sort)
DESKTOP_SRCS := $(shell cd $(DESKTOP_DIR) && find source -name '*.cpp' | sort)

CORE_OBJS    := $(addprefix $(OBJ_DIR)/core/,$(addsuffix .o,$(CORE_SRCS)))
DESKTOP_OBJS := $(addprefix $(OBJ_DIR)/desktop/,$(addsuffix .o,$(DESKTOP_SRCS)))

CORE_INCLUDES := \
  -I$(CORE_DIR)/include -I$(CORE_DIR)/config -I$(CORE_DIR)/source -I$(CORE_DIR)/synth \
  -I$(CORE_DIR)/libs -I$(CORE_DIR)/libs/Audio -I$(CORE_DIR)/libs/ElapsedMillis \
  -I$(CORE_DIR)/libs/Audio/utility -I$(CORE_DIR)/libs/FT812 -I$(CORE_DIR)/libs/fonts \
  -I$(CORE_DIR)/libs/NewDisplay -I$(CORE_DIR)/interface -I$(CORE_DIR)/effector \
  -I$(CORE_DIR)/FileManager -I$(CORE_DIR)/Exporter -I$(CORE_DIR)/AudioEngine \
  -I$(CORE_DIR)/libs/Audio/Polyverb -I$(CORE_DIR)/AudioEngine/ConstantValues \
  -I$(CORE_DIR)/AudioEngine/MasterEnhancer \
  -I$(UTIL_DIR)/include -I$(UTIL_DIR)/3rdparty -I$(SYNTH_DIR)/include

DESKTOP_INCLUDES := -I$(DESKTOP_DIR)/source $(CORE_INCLUDES)

# Mini feature set, no FEATURE_SYNTH, no Teensy/Kinetis defines (those enable ARM asm).
DEFINES := -DPLATFORM_IMX -DTRACKER_MINI -DPLATFORM_DESKTOP \
  -DAUDIO_PERFORMANCE_INFO_ENABLED -DMEMORY_USAGE_INFO_ENABLED -DUSB_MIDI -DLAYOUT_US_ENGLISH \
  $(FW_VERSION_DEFINES)

# -funsigned-char: the firmware is built for ARM, where plain char is unsigned.
COMMON_FLAGS := -MMD -MP -g -O1 -funsigned-char -fno-common $(EXTRA_FLAGS)
# Vendored and legacy code is noisy; keep the warnings that flag portability bugs.
WARN_FLAGS   := -Wno-everything -Wreturn-type -Wpointer-to-int-cast -Wint-to-pointer-cast -Wformat
CXX_FLAGS    := -std=gnu++20 -include cstdint -fno-rtti -fno-exceptions

TARGET := $(BUILD_DIR)/meadowlark

.PHONY: all clean
.DEFAULT_GOAL := all

all: $(TARGET)

$(TARGET): $(CORE_OBJS) $(DESKTOP_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/core/%.cpp.o: $(CORE_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXX_FLAGS) $(COMMON_FLAGS) $(WARN_FLAGS) $(DEFINES) $(CORE_INCLUDES) -c -o $@ $<

$(OBJ_DIR)/core/%.c.o: $(CORE_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_FLAGS) $(WARN_FLAGS) $(DEFINES) $(CORE_INCLUDES) -c -o $@ $<

$(OBJ_DIR)/desktop/%.cpp.o: $(DESKTOP_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXX_FLAGS) $(COMMON_FLAGS) -Wall -Wextra $(DEFINES) $(DESKTOP_INCLUDES) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)

-include $(CORE_OBJS:.o=.d) $(DESKTOP_OBJS:.o=.d)
