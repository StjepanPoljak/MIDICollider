# make             init, midid, recvd for the laptop (build/)
# make image       SD card image for the Pi (output/images/sdcard.img);
#                  on a fresh clone this does everything, including make piano
# make piano       download the Salamander Grand Piano and prepare its samples
# make synthdefs   compile tools/synthdefs.scd into data/synthdefs (needs sclang)
# make flash DEV=/dev/mmcblk0   write the image to an SD card (asks for sudo)
# make verify DEV=/dev/mmcblk0  re-check a written card against the image

BR_VERSION = 2026.02.3
BR_DIR     = buildroot
BR_OUT     = output
BR_MAKE    = $(MAKE) -C $(BR_DIR) O=$(CURDIR)/$(BR_OUT) BR2_EXTERNAL=$(CURDIR)/buildroot-external

# Salamander Grand Piano, CC BY 3.0 (see data/samples/README.txt)
PIANO_URL    = https://freepats.zenvoid.org/Piano/SalamanderGrandPiano/SalamanderGrandPiano-SFZ+FLAC-V3+20200602.tar.gz
PIANO_SHA256 = b7760e168494cf095344e217b0af013fc449ad033abbbdf1c65211cf11dc038b
PIANO_TAR    = dl/salamander.tar.gz
PIANO_DIR    = dl/SalamanderGrandPiano-SFZ+FLAC-V3+20200602
PIANO_STAMP  = dl/piano.stamp

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
image: $(BR_OUT)/.config $(PIANO_STAMP)
	$(BR_MAKE) midicollider-rebuild all
	@echo "SD card image: $(BR_OUT)/images/sdcard.img"

# 707 MB download; only the 4 velocity layers prepare-piano uses get extracted.
$(PIANO_TAR):
	mkdir -p dl
	curl -fL --retry 3 -o $@.part '$(PIANO_URL)'
	echo '$(PIANO_SHA256)  $@.part' | sha256sum -c --quiet
	mv $@.part $@

$(PIANO_STAMP): $(PIANO_TAR) tools/prepare-piano
	tar -xzf $(PIANO_TAR) -C dl --wildcards '*/readme.txt' \
		$(foreach l,4 8 12 16,'*/samples/*v$(l).flac')
	tools/prepare-piano $(PIANO_DIR)
	touch $@

piano: $(PIANO_STAMP)

synthdefs:
	QT_QPA_PLATFORM=offscreen sclang tools/compile-synthdefs.scd

flash:
	tools/flash $(DEV)

verify:
	tools/verify-card $(DEV)

.PHONY: all clean image piano synthdefs flash verify
