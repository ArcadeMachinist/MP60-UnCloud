SDK_ROOT ?= $(wildcard ../../tools/lede17/lede-sdk-*)
TOOLCHAIN := $(wildcard $(SDK_ROOT)/staging_dir/toolchain-*)
CROSS_CC := $(TOOLCHAIN)/bin/mips-openwrt-linux-gcc
CC ?= $(CROSS_CC)
CFLAGS ?= -Os -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
LDFLAGS ?=
SOURCES = main.c util.c hardware.c config.c wifi.c http.c mqtt.c ha.c build/ui.c
TARGET = build/mp60d

.PHONY: all clean self-test
all: $(TARGET)

$(TARGET): $(SOURCES) mp60d.h | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)

build:
	mkdir -p build

build/ui.c: ui.html embed_ui.py | build
	python3 embed_ui.py ui.html $@

self-test: $(TARGET)
	$(TARGET) --self-test

clean:
	rm -rf build
