############################################################################
# apps/plant-companion/ai_module/Makefile
#
# AI 模块编译配置
############################################################################

# AI 模块源文件

ifeq ($(CONFIG_PLANT_AI_MODULE),y)

# 公共模块
CSRCS += ai_module/ai_common.c

# 文字对话模块
ifeq ($(CONFIG_PLANT_AI_TEXT_CHAT),y)
CSRCS += ai_module/text_chat/text_chat.c
endif

# 图像检测模块
ifeq ($(CONFIG_PLANT_AI_IMAGE_DETECT),y)
CSRCS += ai_module/image_detect/image_detect.c
endif

# AI 分析模块
ifeq ($(CONFIG_PLANT_AI_ANALYSIS),y)
CSRCS += ai_module/ai_analysis/ai_analysis.c
endif

endif # CONFIG_PLANT_AI_MODULE
