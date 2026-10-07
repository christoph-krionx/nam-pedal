TARGET = NAMPedalA2
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
CFLAGS += -pipe -ffast-math -fno-unroll-loops -ftree-vectorize -DNAM_MAX_BUFFER_SIZE=48 -DNAM_DTCM='__attribute__((section(".dtcmram_bss")))'
CPPFLAGS += -pipe -DNAM_MAX_BUFFER_SIZE=48
