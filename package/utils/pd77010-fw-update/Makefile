include $(TOPDIR)/rules.mk

PKG_NAME:=pd77010-fw-update
PKG_VERSION:=1.0
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/pd77010-fw-update
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=PD77010 PoE Controller Firmware Updater
  DEPENDS:=+kmod-i2c-core
endef

define Package/pd77010-fw-update/description
  Firmware download and replace utility for the Microchip PD77010 PoE
  Power Management Controller. Communicates over I2C using the BT Serial
  Communication Protocol (DS50003874C) to erase and reprogram the
  controller's NVM application section from a Microchip HEX file.
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/pd77010-fw-update/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/pd77010-fw-update $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,pd77010-fw-update))
