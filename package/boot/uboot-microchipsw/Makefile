include $(TOPDIR)/rules.mk

PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/novarq/u-boot.git
PKG_SOURCE_DATE:=2026-03-25
PKG_SOURCE_VERSION:=2165dbed95a92aae757021ed07feae40b0a0deb7
PKG_MIRROR_HASH:=88544cd1f0fe8a0bd6486cbfe28eb9fe3c0cac2feaf99225d73d920fceaf81b1

PKG_MAINTAINER:=Robert Marko <robert.marko@sartura.hr>

include $(INCLUDE_DIR)/u-boot.mk
include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/kernel.mk

define U-Boot/Default
  BUILD_TARGET:=microchipsw
  HIDDEN:=1
  UBOOT_IMAGE:=u-boot.bin
endef

define U-Boot/ev23x71a
  NAME:=Microchip EV23X71A
  BUILD_DEVICES:=microchip_ev23x71a
  BUILD_SUBTARGET:=lan969x
  UBOOT_CONFIG:=microchip_ev23x71a
endef

define U-Boot/tactical-1000
  NAME:=Novarq Tactical 1000
  BUILD_DEVICES:=novarq_tactical-1000
  BUILD_SUBTARGET:=lan969x
  UBOOT_CONFIG:=novarq_tactical_1000
endef

UBOOT_TARGETS:= \
	ev23x71a \
	tactical-1000

define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(CP) $(PKG_BUILD_DIR)/$(UBOOT_IMAGE) $(STAGING_DIR_IMAGE)/$(BUILD_VARIANT)-$(UBOOT_IMAGE)
endef

$(eval $(call BuildPackage/U-Boot))
