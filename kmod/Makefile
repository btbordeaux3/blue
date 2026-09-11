# Kbuild makefile for the espnet modules.
#
# Build against the running kernel:
#     make
# (optionally)
#     sudo make modules_install
#
# Cross-build for a target SoC:
#     make ARCH=arm64 KDIR=/path/to/kernel/source or headers dir

obj-m := espnet_core.o espnet_usb.o espnet_vbus.o

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all modules_install clean