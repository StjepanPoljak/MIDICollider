# make             init, midid, recvd for the laptop (build/)
# make image       SD card image for the Pi (output/images/sdcard.img)
# make synthdefs   compile tools/synthdefs.scd into data/synthdefs (needs sclang)
# make flash DEV=/dev/mmcblk0   write the image to an SD card (asks for sudo)
# make verify DEV=/dev/mmcblk0  re-check a written card against the image

BR_VERSION = 2026.02.3
BR_DIR     = buildroot
BR_OUT     = output
BR_MAKE    = $(MAKE) -C $(BR_DIR) O=$(CURDIR)/$(BR_OUT) BR2_EXTERNAL=$(CURDIR)/buildroot-external

all:
	$(MAKE) -C src

clean:
	$(MAKE) -C src clean

$(BR_DIR):
	git clone --depth 1 -b $(BR_VERSION) https://gitlab.com/buildroot.org/buildroot.git $@

$(BR_OUT)/.config: | $(BR_DIR)
	$(BR_MAKE) midicollider_rpi3_defconfig

# The programs in src/ are a Buildroot "local" package: changes to them need
# a rebuild of that package before the image picks them up.
image: $(BR_OUT)/.config
	$(BR_MAKE) midicollider-rebuild all
	@echo "SD card image: $(BR_OUT)/images/sdcard.img"

synthdefs:
	QT_QPA_PLATFORM=offscreen sclang tools/compile-synthdefs.scd

flash:
	tools/flash $(DEV)

verify:
	tools/verify-card $(DEV)

.PHONY: all clean image synthdefs flash verify
