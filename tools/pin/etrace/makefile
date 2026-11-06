PIN_ROOT    ?= /home/dmk/fedora/pin
CONFIG_ROOT := $(PIN_ROOT)/source/tools/Config
include $(CONFIG_ROOT)/makefile.config
TOOL_CXXFLAGS += -Wno-maybe-uninitialized
TOOL_ROOTS := etrace
include $(CONFIG_ROOT)/makefile.default.rules
