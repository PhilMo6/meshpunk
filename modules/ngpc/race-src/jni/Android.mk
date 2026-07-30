LOCAL_PATH := $(call my-dir)

DRZ80                    := 0
DEBUG                    := 0
FLAGS                    :=

CORE_DIR := $(LOCAL_PATH)/..

include $(CORE_DIR)/Makefile.common

COREFLAGS := $(INCFLAGS) $(FLAGS) -DWANT_CRC32
COREFLAGS += -D__LIBRETRO__ -D_MAX_PATH=2048

GIT_VERSION := " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
  COREFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

include $(CLEAR_VARS)
LOCAL_MODULE       := retro
LOCAL_SRC_FILES    := $(SOURCES_CXX) $(SOURCES_C)
LOCAL_CFLAGS       := $(COREFLAGS)
LOCAL_CXXFLAGS     := $(COREFLAGS)
LOCAL_LDFLAGS      := -Wl,-version-script=$(CORE_DIR)/libretro/link.T
LOCAL_CPP_FEATURES := exceptions
include $(BUILD_SHARED_LIBRARY)
