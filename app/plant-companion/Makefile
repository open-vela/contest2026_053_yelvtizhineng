############################################################################
# apps/plant-companion/Makefile
#
# Plant Companion Application — AI smart plant care system
############################################################################

include $(APPDIR)/Make.defs

# hal/hal_gpio.h 直接包的是 arch 的 esp32s3_gpio.h（apps 默认不在包含路径里），
# 自动执行层的继电器驱动要用到，这里把 arch 的 chip 目录加进来。
ifneq ($(wildcard $(TOPDIR)/arch/xtensa/src/esp32s3),)
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3
endif

# Main source
MAINSRC = main/app_main.c

# UI layer (product-mode LVGL screens/widgets/theme) — see docs/ARCHITECTURE.md
ifeq ($(CONFIG_PLANT_UI),y)
CSRCS += ui/ui_app.c
CSRCS += ui/theme/theme_plant.c
CSRCS += ui/widgets/widget_topbar.c
CSRCS += ui/widgets/widget_bottomnav.c
CSRCS += ui/screens/screen_home.c
CSRCS += ui/screens/screen_data.c
CSRCS += ui/screens/screen_camera.c
CSRCS += ui/screens/screen_diagnose.c
CSRCS += ui/screens/screen_voice.c
CSRCS += ui/screens/screen_tasks.c
CSRCS += ui/screens/screen_diary.c
# 阶段 A：OTA 升级 UI（二级页 + worker 服务），依赖 CONFIG_ESP32S3_WIFI（ota.c）
ifeq ($(CONFIG_ESP32S3_WIFI),y)
CSRCS += ui/screens/screen_ota.c
CSRCS += services/ota_service.c
# 网络设置页（扫描 SSID + 输密码连接）—— 依赖 wifi_manager
ifeq ($(CONFIG_PLANT_WIFI_MANAGER),y)
CSRCS += ui/screens/screen_wifi.c
endif
endif
# 业务服务层
CSRCS += services/sensor_service.c
CSRCS += services/plant_state.c
CSRCS += services/record_service.c
ifeq ($(CONFIG_PLANT_AI_MODULE),y)
CSRCS += services/ai_service.c
# ⚠️ 3C-2：server_bridge 现在也被图像识别（拍照→服务器→MiMo）使用，
# 不再只属于语音（AI_VOICE）。AI_MODULE 就够（依赖 WiFi+服务器中转）。
CSRCS += services/server_bridge.c
CSRCS += services/actuator_service.c
endif
ifeq ($(CONFIG_PLANT_AI_VOICE),y)
CSRCS += services/voice_service.c
endif
# 中文子集字体（lv_font_conv 生成，见 ui/assets/fonts/README）
CSRCS += ui/assets/fonts/lv_font_plant_zh_12.c
CSRCS += ui/assets/fonts/lv_font_plant_zh_16.c
CSRCS += ui/assets/fonts/lv_font_plant_zh_19.c
CSRCS += ui/assets/fonts/lv_font_plant_zh_25.c
# emoji 单色子集（OpenMoji-black-glyf.ttf 经 lv_font_conv 生成，
# 2026-09-03：UI 真 emoji 渲染，zh_font.c fallback 链尾）
CSRCS += ui/assets/fonts/lv_font_emoji_16.c
CSRCS += ui/assets/fonts/lv_font_emoji_20.c
CSRCS += ui/assets/fonts/lv_font_emoji_24.c
CSRCS += ui/assets/fonts/lv_font_emoji_32.c
# 中文字体统一加载层（CLAUDE_SYSTEM.md §14：数据源抽象 + 缺字诊断）
CSRCS += ui/assets/fonts/zh_font.c
endif

# Component source files
ifeq ($(CONFIG_PLANT_GESTURE_SENSOR),y)
CSRCS += components/gesture_sensor/gesture_sensor.c
endif

ifeq ($(CONFIG_PLANT_UI_PANEL),y)
CSRCS += components/ui_panel/lcd_st7796.c
endif

ifeq ($(CONFIG_PLANT_VOICE_AGENT),y)
CSRCS += components/voice_agent/es8311.c
CSRCS += components/voice_agent/es7210.c
CSRCS += components/voice_agent/voice_agent.c
endif

ifeq ($(CONFIG_PLANT_SYSTEM_MONITOR),y)
CSRCS += components/system_monitor/battery_monitor.c
endif

ifeq ($(CONFIG_PLANT_SD_CARD),y)
CSRCS += components/system_monitor/sd_card.c
CSRCS += components/system_monitor/device_cfg.c
endif

ifeq ($(CONFIG_ESP32S3_WIFI),y)
CSRCS += ota/ota.c
endif

ifeq ($(CONFIG_PLANT_CAMERA_CAPTURE),y)
CSRCS += components/camera_capture/camera_capture.c
endif

ifeq ($(CONFIG_PLANT_SOIL_SENSOR),y)
CSRCS += components/sensor_driver/soil_sensor.c
endif

# Communication source files
ifeq ($(CONFIG_PLANT_WIFI_MANAGER),y)
CSRCS += communication/wifi_manager/wifi_manager.c
endif

# AI module source files
ifeq ($(CONFIG_PLANT_AI_MODULE),y)
CSRCS += ai_module/ai_common.c
CSRCS += ai_module/ai_chat/ai_chat.c
CSRCS += ai_module/ai_image/ai_image.c
CSRCS += ai_module/ai_engine/ai_engine.c
ifeq ($(CONFIG_PLANT_AI_VOICE),y)
CSRCS += ai_module/ai_voice/ai_voice.c
CSRCS += ai_module/ai_voice/ai_ns.c
# esp_sr 神经网络降噪（NSNet2）——预编译库 + NuttX 适配层
# 库在 flat build 下经下方 all:: 规则并入 libapps.a（ar -M addlib）
CSRCS += components/voice_agent/esp_sr/esp_sr_shim.c
CSRCS += components/voice_agent/esp_sr/srmodel_shim.c
CSRCS += components/voice_agent/esp_sr/cjson/cJSON.c
CSRCS += components/voice_agent/esp_sr/model_data/srmodels_data.c
ESP_SR_LIBS := $(APPDIR)/plant-companion/components/voice_agent/esp_sr/lib/libnsnet.a
ESP_SR_LIBS += $(APPDIR)/plant-companion/components/voice_agent/esp_sr/lib/libdl_lib.a
ESP_SR_LIBS += $(APPDIR)/plant-companion/components/voice_agent/esp_sr/lib/libhufzip.a
ESP_SR_LIBS += $(APPDIR)/plant-companion/components/voice_agent/esp_sr/lib/libc_speech_features.a
endif
endif

# Program registration for NSH builtin
PROGNAME = $(CONFIG_PLANT_COMPANION_PROGNAME)
PRIORITY = $(CONFIG_PLANT_COMPANION_PRIORITY)
STACKSIZE = $(CONFIG_PLANT_COMPANION_STACKSIZE)
MODULE = $(CONFIG_PLANT_COMPANION)

include $(APPDIR)/Application.mk

# ---------------------------------------------------------------------------
# flat build：把 esp_sr 预编译库扁平化并入 libapps.a
#
# NuttX flat build 下 Application.mk 的 EXTRA_LIBS 仅在 BUILD_MODULE=y
# 时生效；普通 flat build 里各 app 通过 .built 目标把目标文件
# `ar rcs` 追加进 libapps.a（只替换同名成员，不清空）。这里追加一个
# `all::` 双冒号规则（定义在 Application.mk 之后，故在其之后执行），
# 用 ar -M MRI 脚本把 esp_sr 各 .a 的成员（.c.obj 后缀，与其它 app 的
# .o 无命名冲突）展开进 libapps.a，并刷新符号索引。
# ---------------------------------------------------------------------------
# ───────────────────────────────────────────────────────────────────────
# NS（esp_sr NSNet2）开关——2026-08-25 改为 Kconfig 驱动：
#   CONFIG_PLANT_VOICE_NS=y  →  正常 NS 版（编 ai_ns/esp_sr shim、合并
#   预编译库、ai_voice.c 走 NS 处理）
#   默认（n）                →  无 NS 版（完全移除 esp_sr/NSNet2，
#   ai_voice.c 跳过 NS 调用 + -DPLANT_NO_NS）
# ⚠️ 之前用 make 环境变量 PLANT_NO_NS=1 传递不可靠（build.sh 的子 make
# 不继承），改用 Kconfig（构建系统标准）。PSRAM 问题解决前默认 n 保证可用。
# ───────────────────────────────────────────────────────────────────────
ifneq ($(CONFIG_PLANT_VOICE_NS),y)
CSRCS := $(filter-out \
  ai_module/ai_voice/ai_ns.c \
  components/voice_agent/esp_sr/esp_sr_shim.c \
  components/voice_agent/esp_sr/srmodel_shim.c \
  components/voice_agent/esp_sr/cjson/cJSON.c \
  components/voice_agent/esp_sr/model_data/srmodels_data.c \
  ,$(CSRCS))
ESP_SR_LIBS :=
CFLAGS += -DPLANT_NO_NS=1
endif

ifneq ($(strip $(ESP_SR_LIBS)),)
# ⚠️ 2026-08-24 修复：合并必须与其它 app 的 `ar rcs` 一样走 flock 串行！
# 原实现裸调 `ar -M`，并行构建时与其它 app 的 ARLOCK(flock) 写 libapps.a
# 竞态 → 归档成员损坏（实测 nsh_consolemain.o "file format not recognized"、
# 链接报 nsh_initialize/g_builtins 未定义）。修复：整个 open/create+addlib
# 用同一把 `$(BIN).lock` 锁住，并在合并后 ranlib 刷新符号索引。
all::
	@echo "Merge esp_sr prebuilt libs into libapps.a (flock)"
	$(Q) flock "$(call CONVERT_PATH,$(BIN)).lock" sh -c ' \
	  if [ -f "$(call CONVERT_PATH,$(BIN))" ]; then \
	    { echo "open $(call CONVERT_PATH,$(BIN))"; \
	      for f in $(ESP_SR_LIBS); do echo "addlib $$f"; done; \
	      echo "save"; echo "end"; } | $(firstword $(AR)) -M; \
	  else \
	    { echo "create $(call CONVERT_PATH,$(BIN))"; \
	      for f in $(ESP_SR_LIBS); do echo "addlib $$f"; done; \
	      echo "save"; echo "end"; } | $(firstword $(AR)) -M; \
	  fi'
	$(Q) flock "$(call CONVERT_PATH,$(BIN)).lock" $(firstword $(AR)) s "$(call CONVERT_PATH,$(BIN))"
endif
