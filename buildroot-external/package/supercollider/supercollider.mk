################################################################################
#
# supercollider
#
################################################################################

SUPERCOLLIDER_VERSION = 3.14.1
SUPERCOLLIDER_SOURCE = SuperCollider-$(SUPERCOLLIDER_VERSION)-Source.tar.bz2
SUPERCOLLIDER_SITE = https://github.com/supercollider/supercollider/releases/download/Version-$(SUPERCOLLIDER_VERSION)
SUPERCOLLIDER_LICENSE = GPL-3.0+
SUPERCOLLIDER_LICENSE_FILES = COPYING
SUPERCOLLIDER_DEPENDENCIES = alsa-lib fftw-single libsndfile portaudio
SUPERCOLLIDER_SUPPORTS_IN_SOURCE_BUILD = NO
SUPERCOLLIDER_CMAKE_BACKEND = make

# musl's default thread stack is 128 kB, too small for scsynth. musl takes
# the default from the executable's PT_GNU_STACK size (Alpine does the same).
SUPERCOLLIDER_CONF_OPTS = \
	-DCMAKE_EXE_LINKER_FLAGS="$(TARGET_LDFLAGS) -Wl,-z,stack-size=2097152" \
	-DAUDIOAPI=portaudio \
	-DSYSTEM_PORTAUDIO=ON \
	-DSC_QT=OFF \
	-DSC_IDE=OFF \
	-DNO_X11=ON \
	-DSUPERNOVA=OFF \
	-DSC_EL=OFF \
	-DSC_HIDAPI=OFF \
	-DSC_ABLETON_LINK=OFF \
	-DNO_AVAHI=ON \
	-DINSTALL_HELP=OFF \
	-DENABLE_TESTSUITE=OFF \
	-DNATIVE=OFF \
	-DUSE_CCACHE=OFF

# Only the server/ subtree (scsynth and the core UGen plugins), not sclang.
define SUPERCOLLIDER_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(SUPERCOLLIDER_BUILDDIR)/server
endef

define SUPERCOLLIDER_INSTALL_TARGET_CMDS
	cd $(SUPERCOLLIDER_BUILDDIR)/server && \
		DESTDIR=$(TARGET_DIR) $(BR2_CMAKE) -P cmake_install.cmake
endef

$(eval $(cmake-package))
