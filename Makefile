TARGET = NAMPedal

GCC_PATH = /Library/DaisyToolchain/0.2.0/arm/bin

LIBDAISY_DIR = ../../libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

CPP_SOURCES = NAMPedal.cpp
C_SOURCES = nam_model.c

C_INCLUDES = -I.

APP_TYPE = BOOT_QSPI
CPP_STANDARD = -std=gnu++17
OPT = -O2
LDFLAGS = -u _printf_float

include $(SYSTEM_FILES_DIR)/Makefile

# The Terrarium has no SD card, so the NAM model is embedded in QSPI flash.
# model_data.h is regenerated from model.namb whenever the model changes;
# drop in a different model.namb and rebuild to swap models.
model_data.h: model.namb gen_model_data.py
	python3 gen_model_data.py model.namb model_data.h

build/NAMPedal.o: model_data.h

# Daisy audio block size is 48 frames — must match NAMPedal.cpp SetAudioBlockSize.
# NAM_DTCM places weights and small ring buffers in fast DTCM (Cortex-M7).
CFLAGS   += -ffast-math -fno-unroll-loops -ftree-vectorize \
            -DNAM_MAX_BUFFER_SIZE=48 \
            -DNAM_DTCM='__attribute__((section(".dtcmram_bss")))'
CPPFLAGS += -DNAM_MAX_BUFFER_SIZE=48
