TARGET = NAMPedal

GCC_PATH = /Library/DaisyToolchain/0.2.0/arm/bin

LIBDAISY_DIR = ../../libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

CPP_SOURCES = NAMPedal.cpp
C_SOURCES = nam_model.c

C_INCLUDES = -I.

USE_FATFS = 1
APP_TYPE = BOOT_QSPI
CPP_STANDARD = -std=gnu++17
OPT = -O2
LDFLAGS = -u _printf_float

include $(SYSTEM_FILES_DIR)/Makefile

# Daisy audio block size is 48 frames — must match NAMPedal.cpp SetAudioBlockSize.
# NAM_DTCM places weights and small ring buffers in fast DTCM (Cortex-M7).
CFLAGS   += -ffast-math -fno-unroll-loops -ftree-vectorize \
            -DNAM_MAX_BUFFER_SIZE=48 \
            -DNAM_DTCM='__attribute__((section(".dtcmram_bss")))'
CPPFLAGS += -DNAM_MAX_BUFFER_SIZE=48
