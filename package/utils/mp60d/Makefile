include $(TOPDIR)/rules.mk

PKG_NAME:=mp60d
PKG_VERSION:=1.5.1
PKG_RELEASE:=2
PKG_LICENSE:=NONE

include $(INCLUDE_DIR)/package.mk

define Package/mp60d
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Datto MP60 local outlet, metering, HTTP and MQTT daemon
endef

define Package/mp60d/description
  A standalone local controller for the Datto MP60. It provides six-outlet
  relay control, power metrics, a local HTTP UI/API, Tasmota-style MQTT,
  Home Assistant discovery, and Wi-Fi client configuration without cloud
  services.
endef

define Package/mp60d/conffiles
/etc/mp60d.conf
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/Makefile \
		./src/*.c \
		./src/*.h \
		./src/embed_ui.py \
		./src/ui.html \
		./src/mp60d.init \
		$(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		all
endef

define Package/mp60d/install
	$(INSTALL_DIR) $(1)/sbin $(1)/etc/init.d $(1)/etc/rc.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/mp60d $(1)/sbin/mp60d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/mp60d.init $(1)/etc/init.d/mp60d
	$(LN) ../init.d/mp60d $(1)/etc/rc.d/S99mp60d
	$(CP) ./files/. $(1)/
	$(INSTALL_BIN) ./files/etc/uci-defaults/98-mp60-wpad $(1)/etc/uci-defaults/98-mp60-wpad
endef

$(eval $(call BuildPackage,mp60d))
