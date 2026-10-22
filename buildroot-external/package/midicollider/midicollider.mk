################################################################################
#
# midicollider
#
################################################################################

MIDICOLLIDER_VERSION = 1.0
MIDICOLLIDER_SITE = $(BR2_EXTERNAL_MIDICOLLIDER_PATH)/../src
MIDICOLLIDER_SITE_METHOD = local

define MIDICOLLIDER_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) O=$(@D)/build STATIC=1 \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

# The kernel runs /init from the initramfs, and Buildroot's cpio step leaves
# an existing /init alone.
define MIDICOLLIDER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/init $(TARGET_DIR)/init
	$(INSTALL) -D -m 0755 $(@D)/build/midid $(TARGET_DIR)/usr/bin/midid
	$(INSTALL) -D -m 0755 $(@D)/build/recvd $(TARGET_DIR)/usr/bin/recvd
endef

$(eval $(generic-package))
