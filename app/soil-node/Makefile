include $(APPDIR)/Make.defs

# Architecture headers must resolve after manifest linkfile mapping.
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/arch/xtensa/src/esp32

ifeq ($(CONFIG_SOIL_NODE_MQTT_TLS),y)
CFLAGS += -DMQTT_USE_MBEDTLS
SOIL_ESP_HAL_DIR = $(TOPDIR)/arch/xtensa/src/chip/esp-hal-3rdparty
SOIL_MBEDTLS_DIR = $(SOIL_ESP_HAL_DIR)/components/mbedtls
CFLAGS += -Dmbedtls_ssl_write=esp_mbedtls_ssl_write
CFLAGS += -Dmbedtls_ssl_read=esp_mbedtls_ssl_read
CFLAGS += ${INCDIR_PREFIX}$(SOIL_MBEDTLS_DIR)/mbedtls/include
CFLAGS += ${INCDIR_PREFIX}$(SOIL_MBEDTLS_DIR)/mbedtls/library
CFLAGS += ${INCDIR_PREFIX}$(SOIL_MBEDTLS_DIR)/port/include
CFLAGS += ${INCDIR_PREFIX}$(SOIL_ESP_HAL_DIR)/nuttx/esp32/include
CFLAGS += ${INCDIR_PREFIX}$(SOIL_ESP_HAL_DIR)/components/esp_common/include
CFLAGS += ${INCDIR_PREFIX}$(SOIL_ESP_HAL_DIR)/components/esp_hw_support/include
CFLAGS += ${INCDIR_PREFIX}$(SOIL_ESP_HAL_DIR)/components/esp_rom/include
CFLAGS += ${INCDIR_PREFIX}$(SOIL_ESP_HAL_DIR)/components/hal/include
CFLAGS += ${INCDIR_PREFIX}$(SOIL_ESP_HAL_DIR)/components/soc/include
CFLAGS += ${INCDIR_PREFIX}$(SOIL_ESP_HAL_DIR)/components/soc/esp32/include
CFLAGS += $(DEFINE_PREFIX)MBEDTLS_CONFIG_FILE="<mbedtls/esp_config.h>"
endif

CSRCS += soil_config.c
CSRCS += soil_http.c
CSRCS += soil_mqtt.c
CSRCS += soil_power.c
CSRCS += soil_sensor.c
CSRCS += soil_state.c
CSRCS += soil_time.c
CSRCS += soil_wifi.c
MAINSRC = soil_node_main.c

PROGNAME = $(CONFIG_SOIL_NODE_PROGNAME)
PRIORITY = $(CONFIG_SOIL_NODE_PRIORITY)
STACKSIZE = $(CONFIG_SOIL_NODE_STACKSIZE)
MODULE = $(CONFIG_SOIL_NODE)

include $(APPDIR)/Application.mk
