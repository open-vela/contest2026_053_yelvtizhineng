/****************************************************************************
 * apps/plant-companion/components/voice_agent/voice_agent.c
 *
 * 外设层：音频子系统（ES8311 DAC + ES7210 ADC + I2S0）的初始化与自检。
 *
 * 职责边界（架构约定）：
 *   - 本层只负责"让外设正常运行"：码片寄存器配置、I2S 时钟/DMA、自检
 *   - ai_module/ai_voice（AI 应用层）只调用本层 API，不做外设寄存器操作
 *   - 本层不包含 AI 逻辑（录音诉求 / 语音回复 / 云端交互）
 *
 * 调用链：
 *   voice_agent_init()      → es8311_init/set_format/start + es7210_init + hal_i2s_init
 *   voice_agent_mic_test()  → 采集原始 I2S RX 并统计（外设级麦克风自检）
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>

#include <nuttx/clock.h>
#include <nuttx/i2c/i2c_master.h>

#include "../../hal/hal_i2c.h"
#include "../../hal/hal_i2s.h"
#ifdef CONFIG_PLANT_AUDIO_AW88298
#  include "aw88298.h"
#else
#include "es8311.h"
#endif
#include "es7210.h"
#include "voice_agent.h"

/* I2C 总线初始化（内核符号，链接时解析） */

extern FAR struct i2c_master_s *esp32s3_i2cbus_initialize(int bus);

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
extern int board_cores3_audio_power(bool enable);
#endif


#define VA_I2C_BUS       0
#define VA_SAMPLE_RATE   24000        /* I2S/码片采样率 */
#define VA_MCLK_HZ       (24000 * 256)  /* 6.154MHz = 160MHz/26（整数分频，
                                         * 零抖动；码片从模式跟随外部时钟） */

static FAR struct i2c_master_s *g_i2c;
static bool g_inited;

/* DAC output volume (0..255).  The board-specific codec maps it to its
 * native register scale.
 * 标度：0x00 = -95.5dB，0xFF = +32dB，步进 0.5dB → 0xBF(191) 约 0dB。
 * 2026-09-11：原值 0xE6(230) 折合数字增益约 +19.5dB，听感过大；
 * 按要求输出音量下调 50%（听感减半 ≈ -10dB）→ 0xD2(210)。
 * 可用 NSH `plant voice vol [0-255]` 运行时微调，不必重烧固件。 */

#ifdef CONFIG_PLANT_AUDIO_AW88298
/* CoreS3 near-full-scale tone/speech at volume32: -18dB, no AW clip latch.
 * Start at this measured level after reset; NSH volume remains adjustable. */
static uint8_t g_dac_volume = 32;
#else
static uint8_t g_dac_volume = 0xD2;
#endif

/****************************************************************************
 * 公共 API：音频外设初始化（幂等）
 *
 * ES8311：上电 → I2S 格式/时钟 → 使能 DAC（含解除 DAC SDP MUTE）→ 音量
 * ES7210：复位 → 时钟 → 从模式 → 电源 → 格式 → MIC 增益 → ADC 使能
 * I2S0：  GPIO 矩阵 + 时钟 + DMA（hal_i2s）
 ****************************************************************************/

int voice_agent_init(void)
{
  int ret;

  if (g_inited)
    {
      return 0;
    }

  /* ⚠️ 2026-08-25：NS 版必须在 I2S DMA 配置【之前】映射 PSRAM！
   * 原来在 ai_ns_init（hal_i2s_init 之后）lazy 映射 → 运行中改 MMU
   * 破坏 I2S DMA → RX 3s 超时。提前到最前面映射，I2S 配置时 PSRAM
   * 已就绪。PLANT_NO_NS 版不链接 esp_sr，符号不存在（条件编译）。 */

#ifndef PLANT_NO_NS
  extern int esp_sr_psram_early_init(void);
  esp_sr_psram_early_init();
#endif

  g_i2c = esp32s3_i2cbus_initialize(VA_I2C_BUS);
  if (g_i2c == NULL)
    {
      fprintf(stderr, "[VoiceAgent] I2C%d init failed\n", VA_I2C_BUS);
      return -ENODEV;
    }

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  ret = board_cores3_audio_power(true);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] CoreS3 audio power failed: %d\n", ret);
      return ret;
    }
#endif

  /* Speaker codec: AW88298 on CoreS3, ES8311 on the original BOX board. */

#ifdef CONFIG_PLANT_AUDIO_AW88298
  ret = aw88298_init(g_i2c);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] AW88298 init failed: %d\n", ret);
      return ret;
    }

  ret = aw88298_set_format(g_i2c, VA_SAMPLE_RATE);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] AW88298 format failed: %d\n", ret);
      return ret;
    }

  ret = aw88298_start(g_i2c);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] AW88298 start failed: %d\n", ret);
      return ret;
    }

  aw88298_set_volume(g_i2c, g_dac_volume);
  aw88298_mute(g_i2c, false);

  printf("[VoiceAgent] AW88298 done, ticks=%lu\n",
         (unsigned long)clock_systime_ticks());
#else
  ret = es8311_init(g_i2c);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] ES8311 init failed: %d\n", ret);
      return ret;
    }

  /* ES8311：I2S 格式 + 时钟（12.288MHz / 24kHz / 16-bit） */

  ret = es8311_set_format(g_i2c, VA_SAMPLE_RATE);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] ES8311 format failed: %d\n", ret);
      return ret;
    }

  /* ES8311：使能 DAC（内部会解除 REG09 bit6 DAC SDP MUTE） */

  ret = es8311_start(g_i2c);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] ES8311 start failed: %d\n", ret);
      return ret;
    }

  es8311_set_volume(g_i2c, g_dac_volume);   /* 见 g_dac_volume */
  es8311_mute(g_i2c, false);

  printf("[VoiceAgent] ES8311 done, ticks=%lu\n",
         (unsigned long)clock_systime_ticks());
#endif

  /* The original BOX configures ES7210 before starting its I2S clock and then
   * restores the analog block below. CoreS3 follows StackChan/es_codec_dev:
   * I2S and MCLK are started first, then the ES7210 open/start sequence runs.
   */

#ifndef CONFIG_ARCH_BOARD_M5STACK_CORES3
  ret = es7210_init(g_i2c, VA_SAMPLE_RATE, VA_MCLK_HZ);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] ES7210 init failed: %d\n", ret);
      return ret;
    }

  printf("[VoiceAgent] ES7210 done, ticks=%lu\n",
         (unsigned long)clock_systime_ticks());
#endif

  /* I2S0：GPIO 矩阵 + 时钟 + DMA + PA 使能 */

  printf("[VoiceAgent] hal_i2s_init entry, ticks=%lu\n",
         (unsigned long)clock_systime_ticks());

  ret = hal_i2s_init();
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] I2S0 init failed: %d\n", ret);
      return ret;
    }

  printf("[VoiceAgent] hal_i2s_init done, ticks=%lu\n",
         (unsigned long)clock_systime_ticks());

  /* 启动 TX 时钟（BCK/WS 持续输出，RX 跟随时钟） */

  ret = hal_i2s_start_tx_clock();
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] TX clock start failed: %d\n", ret);
      return ret;
    }

  printf("[VoiceAgent] tx clock done, ticks=%lu\n",
         (unsigned long)clock_systime_ticks());

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  /* StackChan creates the I2S TDM/standard channels before opening ES7210.
   * Programming the ADC while MCLK is absent leaves its clock/analog state
   * marginal and produces very small microphone samples.
   */

  ret = es7210_init(g_i2c, VA_SAMPLE_RATE, VA_MCLK_HZ);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] ES7210 init failed: %d\n", ret);
      return ret;
    }

  printf("[VoiceAgent] ES7210 done after MCLK, ticks=%lu\n",
         (unsigned long)clock_systime_ticks());
#else
  /* The original BOX ES7210 analogue block must be restored after MCLK is
   * running. */
  ret = es7210_restore_analog(g_i2c);
  if (ret < 0)
    {
      fprintf(stderr, "[VoiceAgent] ES7210 restore analog failed: %d\n", ret);
      return ret;
    }
#endif

  g_inited = true;
  return 0;
}

/****************************************************************************
 * 公共 API：麦克风外设自检（不经过 AI 层）
 *
 * 直接通过 HAL 采集原始 I2S RX，分别测槽0（左=MIC1）和槽1（右=MIC2），
 * 每个槽输出：
 *   - 前 32 个原始采样（hex）→ 看数据模式（全零/削顶/真实波形）
 *   - 非零率/峰值/均值 → 判定信号强度
 * 最后给出结论：麦克风在哪个槽、是否正常、增益是否合适。
 ****************************************************************************/

/****************************************************************************
 * 公共 API：DAC 输出音量（ES8311 REG32）
 *
 * 0x00 = -95.5dB .. 0xFF = +32dB，0.5dB/step，0xBF ≈ 0dB。
 * 初始化前调用只记值；初始化后调用立即写寄存器（免重烧调音）。
 ****************************************************************************/

int voice_agent_set_dac_volume(uint8_t vol)
{
  g_dac_volume = vol;

  if (g_i2c != NULL)
    {
#ifdef CONFIG_PLANT_AUDIO_AW88298
      return aw88298_set_volume(g_i2c, vol);
#else
      return es8311_set_volume(g_i2c, vol);
#endif
    }

  return 0;
}

uint8_t voice_agent_get_dac_volume(void)
{
  return g_dac_volume;
}

int voice_agent_speaker_standby(bool standby)
{
#ifdef CONFIG_PLANT_AUDIO_AW88298
  if(!g_inited||!g_i2c)return 0;
  int rc=aw88298_standby(g_i2c,standby);
  if(rc==0&&!standby)rc=aw88298_set_volume(g_i2c,g_dac_volume);
  return rc;
#else
  return 0;
#endif
}
