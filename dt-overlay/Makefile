# Build the overlay and a merged board DTB for documentation purposes.

DTC ?= dtc
DTS  = blue-vbus.dts

all: blue-vbus.dtbo

%.dtbo: %.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

# Show what the overlay would produce when applied (needs a .dts base to fuse)
preview: blue-vbus.dtbo
	$(DTC) -I dtb -O dts -@ blue-vbus.dtbo | head -40

clean:
	rm -f *.dtbo

.PHONY: all preview clean