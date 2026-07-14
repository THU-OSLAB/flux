ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

ifndef BUILD_DIR
ifneq ($(OUTPUT),)
BUILD_DIR := $(abspath $(OUTPUT))
else
BUILD_DIR := $(ROOT_DIR)/build
endif
else
BUILD_DIR := $(abspath $(BUILD_DIR))
endif

OBJ_DIR := $(BUILD_DIR)/obj
EXEC_DIR := $(BUILD_DIR)
KERNEL_OUT := $(BUILD_DIR)/kernel
CONFIG_DIR := $(OBJ_DIR)

AUTOCONF_H := $(CONFIG_DIR)/include/autoconf.h
FLUX_CONFIG := $(CONFIG_DIR)/.config
