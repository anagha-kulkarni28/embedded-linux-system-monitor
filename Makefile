# make             native build
# make arm         static 32-bit ARM binary  -> sysmon-arm
# make test-arm    build for ARM and run it under qemu-arm
# make install     install to $(DESTDIR)$(PREFIX)/bin
#
# ARM toolchain on Debian/Ubuntu:
#   sudo apt install gcc-arm-linux-gnueabihf qemu-user

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -Os -Wall -Wextra -std=c99
LDFLAGS ?=
TARGET  ?= sysmon
PREFIX  ?= /usr

all: $(TARGET)

$(TARGET): sysmon.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	-$(CROSS_COMPILE)strip $@

arm:
	$(MAKE) TARGET=sysmon-arm CROSS_COMPILE=arm-linux-gnueabihf- LDFLAGS=-static

test-arm: arm
	qemu-arm ./sysmon-arm -n 1 -I lo

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f sysmon sysmon-arm

.PHONY: all arm test-arm install clean
