LOCAL_PATH := $(call my-dir)

# RubberBand 4.0 (high-quality live pitch shifter, GPL-2.0-or-later) is OPTIONAL and not stored in this project.
# * present in jni/rubberband-src/ (single/RubberBandSingle.cpp)  -> built in, pitch engine "High"/"Auto" available
# * missing                                                        -> builds anyway with the built-in Lite pitch shifter only
RB_DIR := $(LOCAL_PATH)/rubberband-src
HAVE_RB := $(wildcard $(RB_DIR)/single/RubberBandSingle.cpp)

# RNNoise v0.1.1 (AI noise suppression, BSD-3-Clause, model embedded in rnn_data.c) is OPTIONAL and not stored in this project.
# * present in jni/rnnoise-src/ (src/denoise.c ...)  -> built as a separate C static library and linked in, "AI" noise suppression available
# * missing                                          -> builds anyway (-DVFX_NO_RNNOISE), the "AI" selector falls back to the Classic engine
# RNNoise refuses to compile with -ffast-math (arch.h #error), so it gets its own module with plain release flags.
RN_DIR := $(LOCAL_PATH)/rnnoise-src
HAVE_RN := $(wildcard $(RN_DIR)/src/denoise.c)
ifneq ($(HAVE_RN),)
include $(CLEAR_VARS)
LOCAL_MODULE        := rnnoise_static
LOCAL_SRC_FILES     := rnnoise-src/src/denoise.c rnnoise-src/src/rnn.c rnnoise-src/src/rnn_data.c rnnoise-src/src/pitch.c rnnoise-src/src/kiss_fft.c rnnoise-src/src/celt_lpc.c
LOCAL_C_INCLUDES    := $(RN_DIR)/include $(RN_DIR)/src
LOCAL_ARM_MODE      := arm
LOCAL_ARM_NEON      := true
LOCAL_CFLAGS        := -O3 -DNDEBUG -fno-fast-math -w
include $(BUILD_STATIC_LIBRARY)
endif

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_MODULE        := HexedVoiceMod
LOCAL_SRC_FILES     := main.cpp mod/logger.cpp mod/config.cpp
LOCAL_C_INCLUDES    += $(LOCAL_PATH)
ifneq ($(HAVE_RB),)
$(info HexedVoiceMod: RubberBand found - building with the high-quality pitch engine)
LOCAL_SRC_FILES     += rubberband-src/single/RubberBandSingle.cpp
LOCAL_C_INCLUDES    += $(RB_DIR)
else
$(info HexedVoiceMod: RubberBand NOT found in jni/rubberband-src - building with the built-in Lite pitch shifter only)
LOCAL_CFLAGS        += -DVFX_NO_RUBBERBAND
endif
ifneq ($(HAVE_RN),)
$(info HexedVoiceMod: RNNoise found - building with AI noise suppression)
LOCAL_STATIC_LIBRARIES += rnnoise_static
LOCAL_C_INCLUDES    += $(RN_DIR)/include
else
$(info HexedVoiceMod: RNNoise NOT found in jni/rnnoise-src - building without AI noise suppression)
LOCAL_CFLAGS        += -DVFX_NO_RNNOISE
endif
LOCAL_CPP_FEATURES  := exceptions rtti
LOCAL_ARM_MODE      := arm
LOCAL_ARM_NEON      := true
# RubberBand's R3 engine is heavy: release flags + NEON matter for stutter-free audio on phones
LOCAL_CFLAGS        += -O3 -DNDEBUG -ffast-math -fno-finite-math-only
LOCAL_CPPFLAGS      += -std=c++17
LOCAL_LDLIBS        += -llog -ldl
include $(BUILD_SHARED_LIBRARY)
