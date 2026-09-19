/****************************************************************************
 * apps/plant-companion/ai_module/ai_voice/ai_voice.c
 *
 * 语音 AI 模块：
 *   - I2C：ES8311(DAC) + ES7210(ADC) 码片初始化
 *   - I2S：通过 hal_i2s（自定义 HAL 层）直接操作 I2S0 外设
 *   - 录音：24kHz DMA → 降采样到 16kHz → WAV 编码
 *   - 播放：WAV 解析 → 24kHz DMA → 喇叭
 *
 * 不再使用 NuttX 的 esp32s3_i2s 驱动（上游未验证，RX DMA 时钟不工作）。
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/sched.h>
#include <nuttx/semaphore.h>

#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <nuttx/semaphore.h>

#include "../../components/voice_agent/es8311.h"
#include "../../components/voice_agent/es7210.h"
#include "../../components/voice_agent/voice_agent.h"
#include "../../hal/hal_i2c.h"
#include "../../hal/hal_i2s.h"
#include "ai_voice.h"
#include "ai_ns.h"

/* I2C bus initializer (lives in kernel, resolved at link time) */

extern FAR struct i2c_master_s *esp32s3_i2cbus_initialize(int bus);

/* Kernel GPIO API (flat build: symbols link directly) */

typedef uint16_t gpio_pinattr_t;
extern int  esp32s3_configgpio(uint32_t pin, gpio_pinattr_t attr);
extern void esp32s3_gpiowrite(int pin, bool value);
extern bool esp32s3_gpioread(int pin);

#define GPIO_2   (1 << 1)

/****************************************************************************
 * 私有定义
 ****************************************************************************/

/* I2C 总线号 */

#define I2C_BUS  0

/* 编解码器 MCLK（从模式跟随外部时钟；HAL 实际输出 160/26=6.154MHz） */

#define AI_VOICE_MCLK (24000 * 256)

/* 初始化标记（避免重复初始化） */

static bool g_initialized = false;

/* RX 数据槽位/周期（idx%g_rx_period==g_rx_phase），录音时首块自动检测。
 * 周期 1=连续(全量)、2/4=TDM 稀疏槽。REG08 修复后码片帧结构会变，
 * 故用密度自适应检测（见 ai_voice_record）。 */
static int g_rx_phase = -1;
static int g_rx_period = 1;

/* 2026-08-25 路径A：qsort 比较器（幅度分位数统计用） */

static int cmp_int16_abs(const void *a, const void *b)
{
  int32_t x = *(FAR const int16_t *)a;
  int32_t y = *(FAR const int16_t *)b;

  return (x > y) - (x < y);
}

/****************************************************************************
 * WAV 头生成（44 字节标准 RIFF WAV）
 ****************************************************************************/

static int wav_put32(FAR uint8_t *p, uint32_t v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
  return 4;
}

static int wav_put16(FAR uint8_t *p, uint16_t v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  return 2;
}

static int wav_header(FAR uint8_t *wav, uint32_t data_bytes)
{
  FAR uint8_t *p = wav;
  uint32_t rate = AI_VOICE_AI_RATE;

  memcpy(p, "RIFF", 4);  p += 4;
  p += wav_put32(p, 36 + data_bytes);
  memcpy(p, "WAVE", 4);  p += 4;
  memcpy(p, "fmt ", 4);  p += 4;
  p += wav_put32(p, 16);
  p += wav_put16(p, 1);              /* PCM */
  p += wav_put16(p, AI_VOICE_CHANNELS);
  p += wav_put32(p, rate);
  p += wav_put32(p, rate * 2);       /* byte rate */
  p += wav_put16(p, 2);              /* block align */
  p += wav_put16(p, AI_VOICE_BITS_PER_SAMPLE);
  memcpy(p, "data", 4);  p += 4;
  p += wav_put32(p, data_bytes);

  return (int)(p - wav);
}

/****************************************************************************
 * 公共 API：初始化
 ****************************************************************************/

int ai_voice_init(void)
{
  int ret;

  if (g_initialized)
    {
      return 0;
    }

  /* 外设层初始化：ES8311 + ES7210 + I2S0（components/voice_agent）
   * AI 应用层不直接操作码片/寄存器 */

  ret = voice_agent_init();
  if (ret < 0)
    {
      return ret;
    }

  g_initialized = true;
  return 0;
}

/****************************************************************************
 * 公共 API：降采样 24kHz → 16kHz（线性插值）
 ****************************************************************************/

uint32_t ai_voice_decimate(FAR int16_t *out, FAR const int16_t *in24,
                           uint32_t in_samples)
{
  uint32_t n = 0;

  /* 24kHz → 16kHz = 2/3 重采样
   * 输出第 j 个样本 = 输入第 j*1.5 个样本（线性插值） */

  for (uint32_t j = 0; ; j++)
    {
      uint32_t pos_fixed = j * 3;
      uint32_t i = pos_fixed / 2;
      uint32_t frac = pos_fixed % 2;

      if (i >= in_samples)
        {
          break;
        }

      if (frac == 0)
        {
          out[n++] = in24[i];
        }
      else if (i + 1 < in_samples)
        {
          out[n++] = (int16_t)(((int32_t)in24[i] + (int32_t)in24[i + 1]) / 2);
        }
      else
        {
          out[n++] = in24[i];
        }
    }

  return n;
}

/****************************************************************************
 * 公共 API：WAV 编码
 ****************************************************************************/

int ai_voice_wav_encode(FAR uint8_t *wav, FAR const int16_t *pcm,
                        uint32_t pcm_bytes)
{
  int hdr = wav_header(wav, pcm_bytes);

  if (pcm != (FAR const int16_t *)(wav + hdr))
    {
      memcpy(wav + hdr, pcm, pcm_bytes);
    }

  return hdr + (int)pcm_bytes;
}

/****************************************************************************
 * 公共 API：录音（麦克风 → 24kHz DMA → 降采样 16kHz → WAV）
 *
 * 流式处理：每次只 malloc 一个 100ms 的 24kHz chunk（~9.6KB），
 * 降采样后直接写入 wav 缓冲区（调用者提供，最大 AI_VOICE_WAV_MAX 字节）。
 *
 * ⚠️ 栈：NSH 任务栈已加大到 32KB（CONFIG_SYSTEM_NSH_STACKSIZE），
 * NSNet2 推理（dl_lib 算子嵌套）在此栈上直接执行，无需独立线程。
 ****************************************************************************/

int ai_voice_record(FAR uint8_t *wav, size_t wav_size, int seconds)
{
  FAR int16_t *chunk48;       /* 24kHz RX 缓冲 */
  FAR int16_t *pcm16;         /* 16kHz PCM 指向 wav+44 */
  uint32_t chunk_samples;     /* 每次 DMA 的采样数 */
  uint32_t want_samples;      /* 总共需要的 24kHz 采样数 */
  uint32_t got = 0;           /* 已收到的 24kHz 采样数 */
  uint32_t n16 = 0;           /* 已产生的 16kHz 采样数 */
  bool first_chunk = true;    /* 首块热身标志（跳过起始瞬态） */
  int ret;

  if (seconds <= 0 || seconds > AI_VOICE_MAX_SECONDS)
    {
      seconds = AI_VOICE_MAX_SECONDS;
    }

  /* 按实际秒数校验缓冲（不要求满 AI_VOICE_WAV_MAX 5s 上限）：
   * 16kHz 单声道 PCM 字节数 = AI_VOICE_AI_RATE×2×seconds，加 44 头。
   * 固件含 LVGL/UI 后堆紧张，按实际秒数分配可省堆（见 app_main.c）。 */

  if (wav_size < 44 + AI_VOICE_AI_RATE * 2 * (size_t)seconds)
    {
      return -ENOSPC;
    }

  /* 诊断：记录堆余量（定位 -ENOMEM / 评估 UI 并发占用） */

  {
    struct mallinfo mi = mallinfo();

  }

  ret = ai_voice_init();
  if (ret < 0)
    {
      return ret;
    }

  /* NSNet2 神经网络降噪初始化（失败不阻断录音，仅降噪失效）
   * ⚠️ PLANT_NO_NS=1（make 二分验证）：完全跳过 NS → 无 NS 版 */

#ifndef PLANT_NO_NS
  if (ai_ns_init() < 0)
    {
      printf("[Voice] NSNet2 init 失败——降噪关闭\n");
    }
#endif


  /* 24kHz 临时缓冲——1000 采样 = 2KB。
   * ⚠️ 2026-08-21 对照实验：与 rate 测试相同的 2000B/读（rate 测试 271 读
   * 全好；旧 1000B/读在 ~12 读后系统死）——先消除"读大小"变量。
   * ⚠️ 每次读取必须 ≤ 2047 采样（4094 字节）：
   * hal_i2s_read 的 RXEOF_NUM 上限 4095 字节，超过会提前返回、
   * 后半缓冲是过期数据。 */

  /* ⚠️ 2026-09-16 相位连续（块长必须是 3 的倍数）：
   * 24kHz→16kHz = 2/3。块长 1000 不是 3 的倍数 → 每块只能输出
   * 1000×2/3=666.67 → 取整 666，块间相位逐步漂移：新块第一个输出
   * 样本与上一块最后一个输出样本在原始时间轴上相隔 2.5 个输入采样
   * （≈104µs）→ 每 41.6ms 一次微拼接，听感是低沉的周期性粗糙。
   * 取 1002 → 1002×2/3=668 整数 → 块间严丝合缝，零漂移零接缝。
   * 改此值务必同步核对 s_chunk48 / s_out 容量与 n3 整数性。 */

  chunk_samples = 1002;

  /* ⚠️⚠️ 2026-09-16 关键修复：不再丢弃"块首/块尾"采样！
   *
   * 旧代码每读 1000 个采样就丢掉开头 128 + 末尾 160 = 288 个，理由是
   * "每次 hal_i2s_read_slot 会 RX 复位重启，产生起始瞬态 + 末尾噪声突发"。
   * 那是 2026-08-21 那版驱动的行为。
   *
   * 现在的 hal_i2s_read_slot 已经改成【连续预读链 + 环形 FIFO】
   * （见 nuttx/arch/xtensa/src/esp32s3/hal_i2s.c）：RX 不再每次调用
   * 重启，数据从连续 DMA 流里按需取，既没有块首阶跃、也没有块尾突发。
   *
   * 而"丢掉再拼接"的代价是致命的：
   *   1) 时间轴被压缩 —— 每 1000 采样只留 712，录音比真实说话快 1.40 倍，
   *      听感就是"被压缩了"；
   *   2) 每 712 采样（约 30ms）一次波形硬拼接 → 咔哒/嗡鸣，辅音被切碎，
   *      转写听不懂（实测云端存档录音：每 474 输出采样处跳变是平均值的
   *      4~5.4 倍，铁证）。
   *
   * 故两个值都归 0：全量保留，时间轴 1:1。
   * 首块热身（first_chunk 丢一次）仍保留，用于跨过 RX 预读链的启动瞬态。 */

  #define RX_SETTLE_SAMPLES 0
  #define RX_TAIL_SAMPLES   0
  chunk48 = (FAR int16_t *)malloc(chunk_samples * 2);
  if (chunk48 == NULL)
    {
      printf("[Voice] chunk48 malloc 失败 (%u B), free=%u\n",
             (unsigned)(chunk_samples * 2),
             (unsigned)mallinfo().fordblks);
      return -ENOMEM;
    }

  /* 16kHz PCM 直接写入 wav+44（省掉一个大缓冲） */

  pcm16 = (FAR int16_t *)(wav + 44);
  want_samples = (uint32_t)seconds * 24000;   /* 官方驱动封装：read_slot 直接返回
                                               * 单声道 MIC1 24k 连续流
                                               *（2026-08-25 迁移官方驱动后） */

  /* 诊断：记录耗时（判断 RX 是否在读真实数据）+ 数据指纹 */

  {
    clock_t t0 = clock_systime_ticks();
    uint32_t want_out = AI_VOICE_AI_RATE * (uint32_t)seconds;  /* 16kHz 输出容量 */

    printf("[Voice] record loop start (want_out=%u)\n", (unsigned)want_out);

    /* ⚠️ 2026-08-21 致命 bug 修复：循环终止条件从"24kHz 输入 got"改为
     * "16kHz 输出 n16"！原逻辑按 got<48000 循环，降采样每块输出 667 个
     * （1000×2/3 向上取整），48 块 = 32016 采样 > wav 容量 32000 →
     * 最后 32 字节写到 malloc 块外 → 堆元数据破坏 → free/malloc 崩溃 →
     * 系统死、回放无声。现在 n16 到 32000 即停，且每块 mi 按剩余容量
     * 限制，保证降采样输出绝不越界。 */

    while (n16 < want_out)
      {
        uint32_t n_raw;
        uint32_t mi;
        uint32_t j;
        uint32_t n3;
        clock_t rt0;
        clock_t rt1;

        /* ⚠️ 第一个 chunk 只做热身（丢弃）——采集开头有起始瞬态
         * （ES7210 HPF/VMID 对输入阶跃的响应），跳过以免污染数据。 */

        if (first_chunk)
          {
            first_chunk = false;
            continue;
          }

        /* 诊断：记录每次读取耗时 + 前几个 chunk 的数据指纹 */

        rt0 = clock_systime_ticks();
        ret = hal_i2s_read_slot(chunk48, chunk_samples * 2, 0);
        rt1 = clock_systime_ticks();

#if 0  /* ⚠️ 诊断打印已关闭：USB-Serial-JTAG 输出积压会阻塞主线程（假卡死） */
        printf("[Voice] RD#%u done ret=%d\n", (unsigned)(got / 1000), ret);
#endif

        if (ret <= 0)
          {
            printf("[Voice] RX error: %d\n", ret);
            break;
          }

#if 0  /* 诊断：前几个 chunk 的数据指纹（输出积压会假卡死，已关闭） */
        if ((got % (32 * 500)) == 0)
          {
            uint32_t csum = 0;
            uint32_t ck;

            for (ck = 0; ck < 64 && ck < (uint32_t)ret / 2; ck++)
              {
                csum += (uint32_t)(uint16_t)chunk48[ck];
              }

            printf("[Voice][DIAG] chunk@%u: 读取耗时=%lu ticks 前64采样指纹=%u\n",
                   (unsigned)got,
                   (unsigned long)(rt1 - rt0),
                   csum);
          }

        /* 心跳（每 32 块）：tick 值证明系统还活着（控制台冻结 ≠ 系统卡死） */

        if ((got % (32 * 500)) == 0)
          {
            printf("[Voice] 心跳: got=%u/%u tick=%lu\n",
                   (unsigned)got, (unsigned)want_samples,
                   (unsigned long)clock_systime_ticks());
          }
#endif

        n_raw = (uint32_t)ret / 2;


        /* ⚠️ 2026-08-21 定案：录音改【单槽】路径！
         * 4 槽全采流带起始 DC 阶跃 + 确定性尖峰（污染槽检测与 WAV）；
         * 单槽（只收 slot0=MIC1）流实测干净（静音均值 0、说话时波形
         * 大幅上升，mictest 验证）。2026-08-25 迁移官方驱动后，
         * read_slot 直接返回单声道 MIC1 24k 连续流，
         * 全量提取后 3:2 线性插值 → 16kHz WAV。 */

        mi = n_raw;

        /* ⚠️ 2026-08-21 修复：每块丢弃开头+末尾瞬态！
         * 1) 开头：每次 hal_i2s_read_slot（RX 复位+重启）产生起始瞬态
         *    （HPF 阶跃响应，实测 7800=+30720 起，持续 ~数十采样）。
         * 2) 末尾：实测每块 864-999 区间有 ~31 个大值（>2000，23% 密度）
         *    ——DMA 传输结束/块边界的噪声突发。
         * 两者都会造成"兹拉/吃啦"咔哒。丢弃开头 128 + 末尾 160 采样。 */

        {
          uint32_t remain_out = want_out - n16;
          uint32_t use_end = n_raw - RX_TAIL_SAMPLES;

          if (use_end > RX_SETTLE_SAMPLES)
            {
              mi = use_end - RX_SETTLE_SAMPLES;
            }
          else
            {
              mi = 0;
            }

          /* 24k 单声道流 → 16k 输出 = 2/3 */

          if (mi > remain_out * 3 / 2)
            {
              mi = remain_out * 3 / 2;
            }
        }

        /* 24kHz → 16kHz：线性插值（每 3 输入 → 2 输出）。
         * 输出 j 对应输入位置 j*1.5（2026-08-25 迁移官方驱动后：
         * read_slot 已是连续单声道 24k 流，直接索引即可）。 */

        n3 = mi * 2 / 3;
        for (j = 0; j < n3; j++)
          {
            uint32_t pos2 = j * 3;               /* j*1.5 的 2 倍定点 */
            uint32_t p0 = pos2 / 2;              /* floor(24k 位置) */
            uint32_t frac = pos2 & 1;            /* 0 或 1（对应 .0/.5） */
            int32_t s0 = chunk48[RX_SETTLE_SAMPLES + p0];
            int32_t s1 = chunk48[RX_SETTLE_SAMPLES + p0 + 1];

            pcm16[n16 + j] = (int16_t)((s0 * (2 - (int32_t)frac) +
                                        s1 * (int32_t)frac) / 2);
          }

        /* ⚠️ 2026-08-24 修复：高通去 DC + esp_sr NSNet2 神经网络降噪。
         * 手工滤波（中值/低通/噪声门）只能对付固定特征噪声，对
         * "语音频段宽带噪声"无效——改用 esp_sr NSNet2（神经网络降噪，
         * esp_nn 优化，S3 实时；RNNoise 纯 C GRU 实测卡死已弃用）。
         * 处理链：高通220Hz（去 DC/电源噪声）→ NSNet2（帧流水线）。 */

        {
          static int32_t hp_y = 0;
          static int32_t hp_prev = 0;
#ifndef PLANT_NO_NS
          static int16_t ns_in[1024];
          static uint32_t ns_in_cnt = 0;
          static uint32_t ns_total = 0;   /* 全局采样计数（写回定位） */
          const int ns_frame = ai_ns_frame_size();  /* NSNet2 帧大小 */
#else
          /* ⚠️ 2026-09-01：无 NS 版已删破坏性滤波链（中值/低通/限幅/软门），
           * 只保留高通 80Hz 去 DC（上面处理），裸 PCM 直传服务器降噪。 */
#endif
          uint32_t hj;

          for (hj = 0; hj < n3; hj++)
            {
              int32_t s = pcm16[n16 + hj];

              /* 高通：α=1/64（截止 ~80Hz），滤 DC + 电源噪声
               * ⚠️ 2026-09-01：原 α=1/16（220Hz）会削男声基频 100-150Hz，
               * 与删破坏性滤波链一起改为 80Hz。 */
              hp_y += s - hp_prev;
              hp_y -= hp_y >> 6;
              s = hp_y;
              hp_prev = s;

#ifndef PLANT_NO_NS
              pcm16[n16 + hj] = (int16_t)s;

              /* 喂入 NSNet2 流水线（ns_frame 采样/帧 @16k） */
              ns_in[ns_in_cnt++] = (int16_t)s;
              ns_total++;

              if (ns_in_cnt == (uint32_t)ns_frame)
                {
                  ai_ns_process_frame(ns_in);
                  /* 降噪结果写回对应输入位置（延迟 ns_frame 采样，原位替换） */
                  memcpy(&pcm16[ns_total - ns_frame], ns_in, ns_frame * 2);
                  ns_in_cnt = 0;
                }
#else
              /* ⚠️ 2026-09-01 删破坏性滤波链（同 stream_record）：11点中值
               * 抹清辅音、软门削词尾、限幅削顶 → 语音变"咕噜"。
               * 只保留高通 80Hz（上面已做，α=1/64），裸 PCM 直传，
               * 降噪由服务器负责（server_bridge.py noisereduce）。 */
              pcm16[n16 + hj] = (int16_t)s;
#endif
            }
        }

        n16 += n3;
        got += mi;
      }

    printf("[Voice] 录音耗时≈%lu ms (got=%u, 期望~%u ms 若RX=24kHz)\n",
           (unsigned long)(clock_systime_ticks() - t0) * 1000 / TICK_PER_SEC,
           (unsigned)got, (unsigned)(seconds * 1000));
  }

  free(chunk48);
  if (n16 == 0)
    {
      return -EIO;
    }

  /* 数据分析：判断录到的是真音频还是全零/静音。
   * 峰值 < 50 → 基本是静音/零（ES7210 没输出/没时钟/通路断）；
   * 峰值上千且有波动 → 真音频。 */

  {
    int32_t ssum = 0;
    int32_t spea = 0;
    uint32_t snonzero = 0;
    uint32_t spike_cnt = 0;
    uint32_t spike_first = 0;
    int16_t  spike_val = 0;
    uint32_t k;

    for (k = 0; k < n16; k++)
      {
        int32_t s = pcm16[k];
        ssum += s;
        if (s != 0)
          {
            snonzero++;
          }

        if (s > spea)
          {
            spea = s;
          }
        else if (-s > spea)
          {
            spea = -s;
          }

        /* 诊断：统计残留尖峰（|s|>2000，限幅后应很少） */
        if (s > 2000 || s < -2000)
          {
            spike_cnt++;
            if (spike_first == 0)
              {
                spike_first = k;
                spike_val = (int16_t)s;
              }
          }
      }

    printf("[Voice] 录音统计: %u 采样 非零=%u 峰值=%d 均值=%ld (%s)\n",
           (unsigned)n16, (unsigned)snonzero, (int)spea,
           (long)(ssum / (int32_t)n16),
           spea > 50 ? "有信号" : "静音/全零 ← ES7210 通路疑点");
    printf("[Voice] 残留尖峰(>2000): %u 个, 首个@采样%u 值=%d\n",
           (unsigned)spike_cnt, (unsigned)spike_first, (int)spike_val);

    /* 2026-08-25 路径A：底噪量化（RMS + |s| 分位数）——定门限的数据依据。
     * 安静时 P99/P999 ≈ 底噪上界；说话时 P50 应远高于 P99。
     * ⚠️ 2026-08-25 优化：qsort 全量排序（malloc 64KB）改为 256 桶直方图
     * （128 步进）——零大分配，与 NS 共存时不挤占内部 RAM（NS 加载后
     * record 的 64KB 分配曾导致内存紧张 → RX 超时）。 */

    {
      static uint32_t hist[256];
      static const uint32_t pidx[4] = {50, 90, 99, 999};
      uint64_t sq = 0;
      uint32_t k2;
      uint32_t cum = 0;
      uint32_t target[4];
      uint32_t pval[4];
      int p;

      memset(hist, 0, sizeof(hist));
      target[0] = n16 / 2;
      target[1] = n16 * 9 / 10;
      target[2] = n16 * 99 / 100;
      target[3] = n16 * 999 / 1000;

      for (k2 = 0; k2 < n16; k2++)
        {
          int32_t s = pcm16[k2];
          int32_t a = (s < 0) ? -s : s;

          sq += (uint64_t)a * (uint64_t)a;
          hist[(uint32_t)a >> 7]++;   /* 128 步进，256 桶覆盖 0-32767 */
        }

      p = 0;
      for (k2 = 0; k2 < 256 && p < 4; k2++)
        {
          cum += hist[k2];

          while (p < 4 && cum > target[p])
            {
              pval[p] = k2 * 128 + 63;   /* 桶中值 */
              p++;
            }
        }

      printf("[Voice] 幅度统计: RMS=%lu | P50=%d P90=%d P99=%d P999=%d\n",
             (unsigned long)((uint32_t)sqrt((double)sq / n16)),
             (int)pval[0], (int)pval[1], (int)pval[2], (int)pval[3]);
    }
  }

  return ai_voice_wav_encode(wav, pcm16, n16 * 2);
}

/****************************************************************************
 * 堆健康守卫（WROOM 512KB SRAM 资源调度策略的一部分）
 *
 * 语音链路本体已全静态化（零大 malloc），唯一堆需求是 DMA apb
 * （~4KB/个）。堆余量 <8KB 时这些分配会偶发失败 → 提前警告，
 * 而不是让用户在"录音没数据/播放失败"里猜原因。
 ****************************************************************************/

static void ai_voice_heap_warn(FAR const char *op)
{
  struct mallinfo mi = mallinfo();

  if (mi.fordblks < 8192)
    {
      printf("[Voice] ⚠️ %s: 堆余量不足 (free=%uB largest=%uB)，"
             "可能偶发失败。建议 plant mem 查看/关闭相机\n",
             op, (unsigned)mi.fordblks, (unsigned)mi.mxordblk);
    }
}

/****************************************************************************
 * 公共 API：流式录音（语音链路，设备零大缓冲）
 *
 * 与 ai_voice_record 同源逻辑（read_slot → 丢头尾 → 3:2 降采样 → 高通
 * → 滤波链），但每 ~100ms 把 16kHz mono PCM 块回调给 cb——不攒 WAV。
 *
 * 实现要点：
 *  - 独立 static 滤波状态（与 record 的函数内 static 互不干扰）；
 *  - 输出块 static（100ms=1600 采样=3200B，不占栈/堆大块）；
 *  - 能量 VAD：回调块 RMS 低于阈值持续 silent_blocks 块 → 提前结束
 *    （silent_blocks=0 禁用，纯固定时长）；
 *  - 外部取消：ai_voice_stream_cancel() 可在任意时刻停止（微信式随放随停）；
 *  - ⚠️ 不用 NSNet2 降噪：其流水线写回依赖连续缓冲，与分块回调冲突
 *    （NS 留待服务器侧或延迟线方案）。
 ****************************************************************************/

static volatile bool g_stream_cancel;
static volatile bool g_stream_finish;

void ai_voice_stream_cancel(void)
{
  g_stream_cancel = true;
}

/* ⚠️ 2026-09-11：把取消标志重新起算。
 * g_stream_cancel 是「播放/录音循环」专用标志，只有循环内部会消费。
 * 录音结束到真正开始播放之间还有一段 TTS 下载，若下载期间用户点了
 * 一下（此时不该算「停止播放」），旧代码会让播放循环第一次判断就
 * break → 播放耗时=0 ticks、喇叭完全没声。开始播放前调本函数清零
 * 即可把「再点=停」的起算点对齐到真正出声那一刻。 */

void ai_voice_stream_reset_cancel(void)
{
  g_stream_cancel = false;
}

/* ⚠️ 2026-08-31 微信式「再点=说完」：结束录音（保留已录内容，正常发送），
 * 区别于 ai_voice_stream_cancel（取消=丢弃）。录音循环看到 finish 提前
 * break，返回已录音时长，voice_worker 照常 finalize 发给 AI。 */

void ai_voice_stream_finish(void)
{
  g_stream_finish = true;
}

/****************************************************************************
 * 采集 / IO 解耦队列（2026-09-16 根因修复：录音"被压缩、不清晰、不完整"）
 *
 * 现场实测（V1.3.4 板卡自证）：
 *   回调里写 SD ：5 秒录音实跑 7330ms，实测输入率 16371Hz（24kHz 的 68%）
 *                 → 每块丢 1/3 采样 = 时间轴压缩 + 转写缺字
 *   同固件不写 SD：实跑 5070ms，实测 23668Hz ≈ 满速、零丢采样
 *   → 结论：采集回调里做慢 I/O（写 SD / 上传网络）会饿死 RX 采集链，
 *     丢采样发生在硬件/DMA 层（不是重采样，也不是滤波器）。
 *
 * 对策：采集线程只把整块样本 memcpy 进环形队列；慢 I/O 交给独立线程。
 *   环 4 块 × ≤2048 采样 = 16KB static。环满则阻塞等待（宁可慢，不丢）。
 *   队列排空 + 线程退出后才算录完 → 调用方的"写文件/上传完成"顺序不变。
 ****************************************************************************/

#define AI_VOICE_IOQ_BLOCKS   12   /* 12×2048 采样 ≈ 1.5s 缓冲（放堆/外部内存，不吃内部 SRAM） */
#define AI_VOICE_IOQ_SAMPLES  2048
#define AI_VOICE_IOQ_BATCH    4    /* 攒 4 块（≈400ms）提交一次，见 IO 线程注释 */
#define AI_VOICE_IOQ_DRAIN_MS 3000 /* 收工期限：超期丢块保命（见 ioq_stop） */

struct ai_voice_ioq_s
{
  FAR int16_t *buf;            /* 运行期分配（优选外部内存，省内部 SRAM） */
  uint32_t len[AI_VOICE_IOQ_BLOCKS];
  volatile uint32_t hd;        /* 生产者写指针（仅采集线程写） */
  volatile uint32_t tl;        /* 消费者读指针（仅 IO 线程写） */
  sem_t    free_sem;           /* 空位数 */
  sem_t    data_sem;           /* 待处理块数 */
  sem_t    exit_sem;           /* 线程退出通知 */
  volatile bool stop;
  ai_voice_stream_cb_t cb;
  void    *arg;
  pthread_t tid;
  bool     started;
  volatile uint32_t deadline;   /* 收工期限（tick；0=不限期） */
  volatile bool     abandoned;  /* 收工超时：线程还在跑，队列暂不可复用 */

  /* ⚠️ 2026-09-16 二修：攒批缓冲 + 诊断计数（防"录音卡在聆听"复发） */
  FAR int16_t *acc;            /* 攒批缓冲（BATCH×2048 采样） */
  uint32_t acc_n;              /* 攒批内已用采样数 */
  volatile uint32_t dbg_pushed;
  volatile uint32_t dbg_batched;
  volatile uint32_t dbg_dropped;
  volatile uint32_t dbg_wait_ms;
};

static struct ai_voice_ioq_s s_ioq;

/* ⚠️ 2026-09-16：环形缓冲不再放 .bss（16KB 内部 SRAM 会挤掉 WiFi/相机的
 * 内部堆，实测把 I2S 的 DMA 缓冲挤到外部 PSRAM 区）。改为运行期一次性
 * 分配：只需 CPU 访问，落在哪儿都能用，优先拿外部内存。 */

static FAR int16_t *s_ioq_ring;

static int ai_voice_ioq_ring_ensure(void)
{
  if (s_ioq_ring != NULL)
    {
      return 0;
    }

  s_ioq_ring = (FAR int16_t *)malloc((size_t)AI_VOICE_IOQ_BLOCKS *
                                     AI_VOICE_IOQ_SAMPLES * sizeof(int16_t));
  if (s_ioq_ring == NULL)
    {
      printf("[Voice] IO 环形缓冲分配失败（%u B）\n",
             (unsigned)(AI_VOICE_IOQ_BLOCKS * AI_VOICE_IOQ_SAMPLES * 2));
      return -ENOMEM;
    }

  /* 攒批缓冲：一次 I/O 提交多块，把 TCP 建连次数降到 1/BATCH */

  if (s_ioq.acc == NULL)
    {
      s_ioq.acc = (FAR int16_t *)malloc((size_t)AI_VOICE_IOQ_BATCH *
                                        AI_VOICE_IOQ_SAMPLES *
                                        sizeof(int16_t));
      if (s_ioq.acc == NULL)
        {
          printf("[Voice] 攒批缓冲分配失败\n");
          return -ENOMEM;
        }
    }

  return 0;
}

/* IO 线程：逐块调用回调——慢 I/O 全部在这里做，不占采集线程 */

static FAR void *ai_voice_ioq_thread(FAR void *param)
{
  FAR struct ai_voice_ioq_s *q = (FAR struct ai_voice_ioq_s *)param;

  for (;;)
    {
      bool pending;
      uint32_t idx;
      uint32_t n;

      nxsem_wait_uninterruptible(&q->data_sem);

      pending = (q->tl != q->hd);

      if (!pending && q->stop)
        {
          break;                     /* 已排空且生产者收工 → 退出 */
        }

      /* ⚠️ 2026-09-16 三修：收工期限（ioq_stop 落的 deadline）。
       * 超期就丢弃剩余块立刻退出——绝不因为一条慢网络 POST 把采集线程
       * 无限拖住（那正是"界面停在聆听、按钮点不动"的现场）。 */

      if (q->stop && q->deadline != 0 &&
          (int32_t)(clock_systime_ticks() - (clock_t)q->deadline) >= 0)
        {
          q->acc_n = 0;              /* 丢掉半批，不再发网络 */
          break;
        }

      if (!pending)
        {
          continue;                  /* 唤醒仅来自 stop 计数 */
        }

      idx   = q->tl;
      n     = q->len[idx];
      q->tl = (idx + 1) & (AI_VOICE_IOQ_BLOCKS - 1);

      /* ⚠️ 2026-09-16 二修（攒批）：原来每块（100ms）发一次 I/O，
       * 也就是每秒 10 次 POST、每次都要新建一条 TCP。手机热点下这会把
       * lwIP 连接池打满；一旦掉线，阻塞 connect 还会把本线程按住几十秒
       * 到几分钟 → 录音队列填满 → 界面永远停在"聆听"。
       * 攒批后请求数降到 1/BATCH，而且空位先还，采集侧完全不受 I/O 影响。 */

      memcpy(q->acc + q->acc_n,
             q->buf + (size_t)idx * AI_VOICE_IOQ_SAMPLES, n * 2);
      q->acc_n += n;
      q->dbg_batched++;

      nxsem_post(&q->free_sem);          /* 空位先还，不等 I/O 做完 */

      if (q->acc_n >= (AI_VOICE_IOQ_BATCH * AI_VOICE_IOQ_SAMPLES) ||
          q->tl == q->hd)
        {
          if (q->cb != NULL && q->acc_n > 0)
            {
              q->cb(q->acc, q->acc_n, q->arg);
            }

          q->acc_n = 0;
        }
    }

  /* 收工前把攒批里剩下的最后一段提交掉（不丢尾） */

  if (q->cb != NULL && q->acc_n > 0)
    {
      q->cb(q->acc, q->acc_n, q->arg);
      q->acc_n = 0;
    }

  nxsem_post(&q->exit_sem);
  return NULL;
}

static int ai_voice_ioq_start(ai_voice_stream_cb_t cb, void *arg)
{
  pthread_attr_t attr;
  int ret;

  if (ai_voice_ioq_ring_ensure() < 0)
    {
      return -ENOMEM;
      return -ENOMEM;
    }

  /* ⚠️ 2026-09-16 三修：上一次收工超时（网络卡）→ 旧线程可能还在跑，
   * 此时绝不能复用同一个队列（两个线程写同一份环形缓冲 = 内存错乱）。
   * 先等它退出（IO 线程看到收工期限会立刻丢块走人），等不到就拒绝
   * 这一轮录音——宁可这轮没声音，也不能让板卡乱掉。 */

  if (s_ioq.abandoned)
    {
      if (nxsem_tickwait(&s_ioq.exit_sem, MSEC2TICK(15000)) < 0)
        {
          printf("[Voice] ⚠ 上一轮 IO 线程仍未退出，本轮语音暂不可用\n");
  s_ioq.dbg_wait_ms = 0;
  s_ioq.deadline    = 0;
  s_ioq.abandoned   = false;
        }

      pthread_join(s_ioq.tid, NULL);
      nxsem_destroy(&s_ioq.free_sem);
      nxsem_destroy(&s_ioq.data_sem);
      nxsem_destroy(&s_ioq.exit_sem);
      s_ioq.abandoned = false;
    }

  s_ioq.buf     = s_ioq_ring;
  s_ioq.hd      = 0;
  s_ioq.tl      = 0;
  s_ioq.stop    = false;
  s_ioq.cb      = cb;
  s_ioq.arg     = arg;
  s_ioq.started = false;
  s_ioq.acc_n   = 0;
  s_ioq.dbg_pushed  = 0;
  s_ioq.dbg_batched = 0;
  s_ioq.dbg_dropped = 0;
  s_ioq.dbg_wait_ms = 0;

  nxsem_init(&s_ioq.free_sem, 0, AI_VOICE_IOQ_BLOCKS);
  nxsem_init(&s_ioq.data_sem, 0, 0);
  nxsem_init(&s_ioq.exit_sem, 0, 0);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);   /* 与 voice_worker 一致 */

  /* ⚠️ 2026-09-16 关键：IO 线程优先级必须低于采集线程。
   * 本板 CONFIG_RR_INTERVAL=200（同优先级轮转片 = 200×10ms = 2s！），
   * 同优先级下写 SD / 发 HTTP 会整片占住 CPU，采集线程醒不过来 →
   * I2S 环形 FIFO（2048 采样≈85ms）溢出丢采样 → 用户听到"变快/不完整/
   * 像被压缩"。降到调用者 -10：采集永远抢占，慢 IO 只吃 CPU 空隙。 */

  {
    struct sched_param sp;
    int pol;

    if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0)
      {
        sp.sched_priority = (sp.sched_priority > 10) ?
                            (sp.sched_priority - 10) : 1;
        pthread_attr_setschedparam(&attr, &sp);
      }
  }
  ret = pthread_create(&s_ioq.tid, &attr, ai_voice_ioq_thread, &s_ioq);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      printf("[Voice] IO 队列线程创建失败: %d（退回同步回调）\n", ret);
      nxsem_destroy(&s_ioq.free_sem);
      nxsem_destroy(&s_ioq.data_sem);
      nxsem_destroy(&s_ioq.exit_sem);
      return -ret;
    }

  s_ioq.started = true;
  return 0;
}

static void ai_voice_ioq_push(FAR const int16_t *pcm, uint32_t samples)
{
  uint32_t idx;

  if (!s_ioq.started || samples == 0)
    {
      return;
    }

  if (samples > AI_VOICE_IOQ_SAMPLES)
    {
      samples = AI_VOICE_IOQ_SAMPLES;      /* 防御：正常块 ≤2048 */
    }

  /* ⚠️ 2026-09-16 二修：环满时改为"有界等待 + 丢弃"，绝不再无限等。
   * 旧实现 nxsem_wait_uninterruptible 一旦碰上 IO 线程被慢网络/掉线
   * 的 connect 按住，采集线程就永远醒不过来 —— 界面一直停在"聆听"，
   * 用户再点按钮也无效（"说完"标志在等待里根本没被检查）。
   * 现在最多等 400ms：等不到就丢掉这一块并留痕，录音照常按时收尾。 */

  {
    clock_t tw0 = clock_systime_ticks();

    for (;;)
      {
        int w = nxsem_tickwait(&s_ioq.free_sem, MSEC2TICK(400));

        if (w >= 0)
          {
            break;
          }

        s_ioq.dbg_dropped++;

        if (s_ioq.dbg_dropped == 1)
          {
            printf("[Voice] ⚠ IO 队列堵住（I/O 跟不上），先丢块保采集\n");
          }

        return;
      }

    {
      uint32_t wt = (uint32_t)((clock_systime_ticks() - tw0) * 1000 /
                               TICK_PER_SEC);

      if (wt > s_ioq.dbg_wait_ms)
        {
          s_ioq.dbg_wait_ms = wt;
        }
    }
  }

  idx = s_ioq.hd;
  memcpy(s_ioq.buf + (size_t)idx * AI_VOICE_IOQ_SAMPLES, pcm,
         samples * 2);
  s_ioq.len[idx] = samples;
  s_ioq.hd = (idx + 1) & (AI_VOICE_IOQ_BLOCKS - 1);
  s_ioq.dbg_pushed++;

  nxsem_post(&s_ioq.data_sem);
}

static void ai_voice_ioq_stop(void)
{
  /* ⚠️ 2026-09-16 三修（"界面永远停在聆听"根治）：这里绝不能无限等。
   * IO 线程可能正卡在一次 POST 里（掉线时 connect/send/recv 各带超时，
   * 最坏叠加十几秒），旧代码 nxsem_wait_uninterruptible 会一直等它，
   * 采集线程于是留在 ai_voice_stream_record 里不出来，界面就永远停在
   * "正在听你说"，再点按钮也无效（g_busy 一直为真）。
   * 现在：①先给 IO 线程一个收工期限（超期直接丢弃剩余块）；
   *       ②本函数最多等 4 秒，超时就放弃等待（保留现场，绝不销毁
   *         正在被别的线程使用的信号量），录音立刻收尾、界面回到
   *         "AI思考中"。真正放弃的队列由 ai_voice_ioq_start 回收。 */

  if (!s_ioq.started)
    {
      return;
    }

  s_ioq.stop = true;
  s_ioq.deadline = clock_systime_ticks() +
                   MSEC2TICK(AI_VOICE_IOQ_DRAIN_MS);
  nxsem_post(&s_ioq.data_sem);             /* 唤醒线程收工 */

  if (nxsem_tickwait(&s_ioq.exit_sem,
                     MSEC2TICK(AI_VOICE_IOQ_DRAIN_MS + 1000)) < 0)
    {
      s_ioq.abandoned = true;
      s_ioq.started   = false;
      printf("[Voice] ⚠ IO 线程 %d ms 内没排空（网络卡住），本轮不再等，"
             "录音照常收尾\n", AI_VOICE_IOQ_DRAIN_MS + 1000);
      return;
    }

  pthread_join(s_ioq.tid, NULL);

  nxsem_destroy(&s_ioq.free_sem);
  nxsem_destroy(&s_ioq.data_sem);
  nxsem_destroy(&s_ioq.exit_sem);

  s_ioq.started = false;
}

int ai_voice_stream_record(ai_voice_stream_cb_t cb, void *arg,
                           int max_seconds, int silent_blocks)
{
  /* 独立滤波状态（stream 专用，与 record 隔离）——
   * ⚠️ 2026-09-01 删破坏性滤波链后仅剩高通去 DC 状态 */
  static int32_t s_hp_y = 0;
  static int32_t s_hp_prev = 0;

  /* 输出块：100ms @16kHz = 1600 采样 = 3200B（static，零分配）
   * ⚠️ 2026-09-02 越界修复：每块输出 n3=474（输入 1000），out_n 序列
   * 0,474,948,1422,1896 → 第 4 块写 s_out[1422..1896) 超 1600 越界 296
   * 个元素（592B）→ 写穿 s_chunk48 头部 → 回调 samples=1896 又越界读
   * → 结构性零值/统计错乱（diag 说话段非零率恒定 69% 的真凶之一）。
   * 容量加大到 2048。⚠️ 2026-09-16 不再丢样本 + 块长改 1002 后，
   * 每块输出 n3=668，out_n 序列 668,1336,2004 → 峰值 2004
   * （仍 ≤ 2048），回调按实际 out_n 统计。
   * 若日后改 chunk_samples，务必同步复核此容量。 */
  static int16_t s_out[2048];

  /* 24kHz 读缓冲（static：WiFi 连接后堆被动态缓冲吃紧，语音链路零 malloc） */
  static int16_t s_chunk48[1002];   /* 与 chunk_samples=1002 同步 */

  FAR int16_t *chunk48;
  uint32_t chunk_samples;
  uint32_t out_n = 0;
  uint32_t want_out;
  uint32_t n16 = 0;
  uint32_t got = 0;
  bool first_chunk = true;
  int silent_cnt = 0;
  int ret;

  /* ⚠️ 2026-09-16 接缝诊断：块拼接处跳变 / 全体相邻跳变。
   * 丢样本或相位断裂 → 新块首样本与上一块末样本的跳变远大于普通
   * 相邻样本跳变（旧版每 1000 采样丢 288 时实测 400~500/100）。
   * 相位连续时应 ≈100~150/100。 */

  uint64_t seam_sum = 0;
  uint32_t seam_cnt = 0;
  uint64_t diff_sum = 0;
  uint32_t diff_cnt = 0;
  uint32_t jump_outlier = 0;   /* |diff| > 8×平均 的离群跳变计数（≈咔哒次数） */
  int32_t  prev_out = 0;
  bool     has_prev = false;

  /* ⚠️ 2026-09-16 三修：自适应静音门限（"说了停不下来"根治）相关状态 */
  int64_t vad_noise  = 0;          /* 噪声地板（块均方；0=还没测到） */
  int32_t vad_thr    = 300 * 300;  /* 当前静音门限（均方） */
  int     vad_speech = 0;          /* 判到人声的块数 */
  bool    ended_silent = false;    /* 是否由静音自动结束 */

  clock_t t_start = clock_systime_ticks();   /* 实时性诊断用 */

  /* FIFO 溢出丢采样基线（本轮增量 = 真正丢掉的采样数） */

  uint32_t drop0 = hal_i2s_rx_drop_count();

  g_stream_cancel = false;   /* 每次录音开始清取消标志 */
  g_stream_finish = false;   /* 每次录音开始清「说完」标志 */

  if (cb == NULL)
    {
      return -EINVAL;
    }

  ai_voice_heap_warn("录音");

  if (max_seconds <= 0 || max_seconds > AI_VOICE_MAX_SECONDS)
    {
      max_seconds = AI_VOICE_MAX_SECONDS;
    }

  ret = ai_voice_init();
  if (ret < 0)
    {
      return ret;
    }

  chunk_samples = 1002;   /* 24kHz 2004B/读（3 的倍数，见 record 注释） */
  chunk48 = s_chunk48;    /* static，零 malloc */

  want_out = AI_VOICE_AI_RATE * (uint32_t)max_seconds;

  /* ⚠️ 2026-09-16：回调（写 SD / 上传网络）改为走独立 IO 线程，
   * 采集线程只 memcpy —— 这是"录音被压缩/不清晰"的根治点。 */

  if (ai_voice_ioq_start(cb, arg) < 0)
    {
      return -ENOMEM;
    }

  while (n16 < want_out)
    {
      uint32_t n_raw;
      uint32_t mi;
      uint32_t j;
      uint32_t n3;

      /* ⚠️ 2026-09-16 二修：硬上限兜底。正常 5s 录音实跑 ~5s；一旦某条
       * 路径被卡住（网络 / DMA / 信号量），这里最多再跑 8 秒就强制收尾，
       * 保证界面绝不会永远停在"聆听"。 */

      if ((clock_systime_ticks() - t_start) >
          (clock_t)(max_seconds + 8) * TICK_PER_SEC)
        {
          printf("[Voice] ⚠ 录音超过硬上限（%d s + 8s），强制收尾\n",
                 max_seconds);
          break;
        }

      /* 外部取消（取消=丢弃，如 THINKING 阶段再点）：
       * 再点按键 → 立即停止录音 */
      if (g_stream_cancel)
        {
          break;
        }

      /* ⚠️ 2026-08-31 微信式「再点=说完」：结束录音（保留已录内容，
       * 返回正时长 → worker 照常 finalize 发给 AI） */
      if (g_stream_finish)
        {
          break;
        }

      if (first_chunk)
        {
          first_chunk = false;
          continue;
        }

      ret = hal_i2s_read_slot(chunk48, chunk_samples * 2, 0);
      if (ret <= 0)
        {
          break;
        }

      n_raw = (uint32_t)ret / 2;

      /* 丢头尾瞬态（同 record：开 RX_SETTLE、末 RX_TAIL） */
      {
        uint32_t remain_out = want_out - n16;
        uint32_t use_end = n_raw - RX_TAIL_SAMPLES;

        if (use_end > RX_SETTLE_SAMPLES)
          {
            mi = use_end - RX_SETTLE_SAMPLES;
          }
        else
          {
            mi = 0;
          }

        if (mi > remain_out * 3 / 2)
          {
            mi = remain_out * 3 / 2;
          }
      }

      /* 24k → 16k 线性插值（同 record） */
      n3 = mi * 2 / 3;
      for (j = 0; j < n3; j++)
        {
          uint32_t pos2 = j * 3;
          uint32_t p0 = pos2 / 2;
          uint32_t frac = pos2 & 1;
          int32_t s0 = chunk48[RX_SETTLE_SAMPLES + p0];
          int32_t s1 = chunk48[RX_SETTLE_SAMPLES + p0 + 1];

          s_out[out_n + j] = (int16_t)((s0 * (2 - (int32_t)frac) +
                                        s1 * (int32_t)frac) / 2);
        }

      /* ⚠️ 2026-09-01 删破坏性滤波链：11点中值（非线性破坏语音，抹清辅音）
       * + 低通（二次平滑）+ 限幅±30000（37.5dB 增益下削顶失真）+
       * 软噪声门（|s|<500 衰减 1/4，削词首/词尾）——实测把语音变成
       * "咕噜咕噜"（用户听 before.wav 完全听不到字）。
       * 保留：高通 80Hz（α=1/64，去 DC/电源噪声，不伤语音频段）。
       * 裸 PCM 直传服务器，降噪由服务器负责（server_bridge.py noisereduce）。 */
      for (j = 0; j < n3; j++)
        {
          int32_t s = s_out[out_n + j];

          /* 高通：α=1/64（~80Hz 去 DC/电源噪声；220Hz 会削男声基频） */
          s_hp_y += s - s_hp_prev;
          s_hp_y -= s_hp_y >> 6;
          s = s_hp_y;
          s_hp_prev = s;

          s_out[out_n + j] = (int16_t)s;

          /* 接缝诊断：j==0 是本读块第一个输出样本，它是否与上一块
           * 最后一个输出样本连续，是"块拼接是否干净"的直接判据 */

          if (has_prev)
            {
              int32_t dj = (int32_t)s - prev_out;

              if (dj < 0)
                {
                  dj = -dj;
                }

              diff_sum += (uint64_t)dj;
              diff_cnt++;

              /* 离群跳变（≈"咔哒"接点数）：与"到当前为止的平均跳变"
               * 比，超过 8 倍记一处。丢采样/环形 FIFO 溢出都会在这里
               * 留下固定节奏的计数，是"听不清"的量化判据。 */

              if (diff_cnt > 64)
                {
                  uint32_t run_avg = (uint32_t)(diff_sum / diff_cnt);

                  if (run_avg > 0 && dj > (int32_t)(run_avg * 8))
                    {
                      jump_outlier++;
                    }
                }

              if (j == 0)
                {
                  seam_sum += (uint64_t)dj;
                  seam_cnt++;
                }
            }

          prev_out = s;
          has_prev = true;
        }

      out_n += n3;
      n16 += n3;
      got += mi;

      /* 输出块满（~100ms=1600 采样）→ 回调 */
      if (out_n >= 1600)
        {
          /* 能量 VAD（2026-08-31 修复：峰值对偶发尖峰太敏感 → 静音永不触发；
           * 改用均方（mean-square）——多数采样幅度低才算静音，单个尖峰不影响）。
           * 静音判定：mean-square < 300²=90000（对应 RMS<300，
           * record 静音基线 RMS≈284 → mean-square≈80656，说话时远大于 90000）。
           * ⚠️ 2026-08-31 二修：原阈值 300*300/1000=90 是笔误（小 1000 倍），
           * 导致只有 RMS<9.5 才判静音 → 静音永不触发、总是录满 5 秒。
           * 连续 silent_blocks 块（1.5s）→ 说完自动停。 */

          int64_t sq = 0;
          uint32_t k;

          for (k = 0; k < out_n; k++)
            {
              sq += (int64_t)s_out[k] * s_out[k];
            }

          if (silent_blocks > 0)
            {
              int32_t msq = (int32_t)(sq / out_n);   /* 均方（非 RMS） */
              int32_t thr;

              /* 噪声地板：快降慢升（比当前低就立刻跟随，高只按 1/64 逼近）
               * —— 说话块不会把地板抬高，停顿时地板马上贴住底噪。 */

              if (vad_noise == 0 || msq < vad_noise)
                {
                  vad_noise = msq;
                }
              else
                {
                  vad_noise += (msq - vad_noise) / 64;
                }

              /* 门限 = 噪声地板 ×4，且夹在 [300², 3000²]：安静房间仍是
               * 原来的 300²，嘈杂环境自动抬高——这是"说了停不下来、要硬等
               * 满 5 秒"的根治点（办公室底噪 RMS≈284 就贴在旧门限上）。 */

              thr = (int32_t)(vad_noise * 4);
              if (thr < 300 * 300)
                {
                  thr = 300 * 300;
                }
              else if (thr > 3000 * 3000)
                {
                  thr = 3000 * 3000;
                }

              vad_thr = thr;

              if (msq < thr)
                {
                  silent_cnt++;
                }
              else
                {
                  silent_cnt = 0;
                  vad_speech++;
                }
            }

          ai_voice_ioq_push(s_out, out_n);
          out_n = 0;

          if (silent_blocks > 0 && silent_cnt >= silent_blocks)
            {
              ended_silent = true;
              break;   /* 静音足够久 → 说完自动停 */
            }
        }
    }

  /* 尾部不足一块的数据也回调（不丢尾） */
  if (out_n > 0 && cb != NULL)
    {
      ai_voice_ioq_push(s_out, out_n);
    }

  /* ⚠️ 2026-09-16 实时性诊断：录音耗时必须 ≈ 录音时长。
   * 若"实时比"明显小于 1000，说明每块丢了采样（时间轴被压缩），
   * 输出会听起来又快又不清楚 —— 这是 2026-09-16 那轮修的 bug 的
   * 现场指示器，以后回归一眼就能看出来。 */

  {
    unsigned long el = (unsigned long)(clock_systime_ticks() - t_start);
    unsigned long el_ms = el * 1000 / TICK_PER_SEC;
    int rec_ms = (int)(n16 * 1000 / AI_VOICE_AI_RATE);

    unsigned long seam_avg = seam_cnt ?
        (unsigned long)(seam_sum / seam_cnt) : 0;
    unsigned long diff_avg = diff_cnt ?
        (unsigned long)(diff_sum / diff_cnt) : 0;
    unsigned long seam_ratio = diff_avg ?
        (seam_avg * 100 / diff_avg) : 0;

    printf("[Voice] 录音 %d ms｜实跑 %lu ms｜实时比 %lu/1000"
           "｜输入 %u 采样(%lu Hz 实测)→输出 %u 采样\n",
           rec_ms, el_ms,
           el_ms * 1000 / (rec_ms > 0 ? (unsigned long)rec_ms : 1),
           (unsigned)got,
           (unsigned long)(el_ms > 0 ?
               (unsigned long)got * 1000UL / el_ms : 0),
           (unsigned)n16);

    /* 接缝比 ≈100~150 = 块间连续；≥300 = 块拼接处有硬跳变（丢样本）
     * 实时比 ≈1000 = 时间轴 1:1；明显 >1000 = 录音被拖慢（有丢样本
     * 或 SD 写拖累采集）。 */

    printf("[Voice] 接缝比 %lu/100（块首跳变 %lu / 平均跳变 %lu，"
           "接缝 %u 处）\n",
           seam_ratio, seam_avg, diff_avg, (unsigned)seam_cnt);

    printf("[Voice] 离群跳变 %u 处（|跳变| > 8×平均，≈咔哒次数）\n",
           (unsigned)jump_outlier);

    /* ⚠️ 2026-09-16 新增：丢采样计数（I2S 环形 FIFO 溢出）。
     * 0 = 采样一个没丢（时间轴完整，语音不变调）；>0 就是"听不清/被
     * 压缩"的量化证据，且直接指向"采集线程被慢 IO 挤掉"这一根因。 */

    printf("[Voice] 丢采样 %lu 个（FIFO 溢出；0 = 时间轴完整）\n",
           (unsigned long)(hal_i2s_rx_drop_count() - drop0));

    printf("[Voice] IO 队列: 入队 %lu / 攒批 %lu / 丢块 %lu / 最长等待 %lu ms\n",
           (unsigned long)s_ioq.dbg_pushed,
           (unsigned long)s_ioq.dbg_batched,
           (unsigned long)s_ioq.dbg_dropped,
           (unsigned long)s_ioq.dbg_wait_ms);

    /* ⚠️ 2026-09-16 三修：把"结束方式"和 VAD 门限打出来——"说完自动停"
     * 是否真的生效，一眼可判（固定门限在嘈杂环境下永不触发）。 */

    printf("[Voice] 结束方式 %s（静音 %d 块 / 门限(均方) %ld / "
           "噪声地板 %ld / 判到人声 %d 块）\n",
           ended_silent ? "静音自动停" : "到点收尾",
           silent_cnt, (long)vad_thr, (long)vad_noise, vad_speech);
  }

  /* 队列排空 + IO 线程退出后才算录完（写文件/上传完成顺序不变） */

  ai_voice_ioq_stop();

  return (int)(n16 * 1000 / AI_VOICE_AI_RATE);   /* 录音时长 ms */
}


/****************************************************************************
 * 公共 API：播放（WAV → 24kHz DMA → 喇叭）
 *
 * 注意：当前 I2S 以 24kHz 运行，16kHz WAV 播放出来会快 3 倍。
 * 后续需要加 16k→48k 上采样。
 ****************************************************************************/

int ai_voice_play(FAR const uint8_t *wav, size_t size)
{
  FAR const uint8_t *p;
  int data_bytes = 0;
  bool found = false;
  int ret;

  if (size < 44 || memcmp(wav, "RIFF", 4) != 0)
    {
      return -EINVAL;
    }

  /* 找 WAV 的 "data" chunk。
   * ⚠️ 必须从 wav+12 开始（跳过 "RIFF"+size+"WAVE"）！
   * 之前从 wav[0] 开始，把 RIFF 头当 chunk 遍历，
   * 一步跳过整个文件 → 永远找不到 data → 静默 -EINVAL → 无声。 */

  p = wav + 12;
  while (p + 8 <= wav + size)
    {
      uint32_t csize = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                       ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);

      if (memcmp(p, "data", 4) == 0)
        {
          data_bytes = (int)csize;
          p += 8;
          found = true;
          break;
        }

      /* 跳过当前 chunk（含奇数字节补齐） */

      p += 8 + csize + (csize & 1);
    }

  if (!found || p + data_bytes > wav + size)
    {
      printf("[Voice] 播放失败: WAV data chunk 未找到/越界 (size=%u)\n",
             (unsigned)size);
      return -EINVAL;
    }


  ret = ai_voice_init();
  if (ret < 0)
    {
      return ret;
    }

  /* 重采样：16kHz 单声道 WAV → 24kHz 立体声帧（L=R），线性插值。
   * I2S TX 以 24kHz 帧 × 2 槽 = 48k 采样/s 输出，原样喂 16k 单声道
   * 会 3 倍速播放（花栗鼠音）。输出帧 j 对应输入位置 j*2/3。 */

  {
    static int16_t in[800];      /* 栈上 1600B 会压栈，改 static */
    static int16_t outbuf[2400]; /* 800 输入采样 → 1200 帧 × 2 槽 */
    int sent = 0;                /* 已消费的输入字节数 */

    while (sent < data_bytes)
      {
        int n_in;
        int n_out = 0;
        int j;

        n_in = (data_bytes - sent) / 2;
        if (n_in > 800)
          {
            n_in = 800;
          }

        /* 防死循环：无有效采样则结束 */

        if (n_in <= 0)
          {
            break;
          }

        memcpy(in, p + sent, (size_t)n_in * 2);

        for (j = 0; j < n_in * 3 / 2; j++)
          {
            uint32_t pos = (uint32_t)j * 2;   /* 24k 帧 j ↔ 16k 位置 2j/3 */
            uint32_t i = pos / 3;
            uint32_t frac = pos % 3;
            int16_t a = in[i];
            int16_t b = (i + 1 < (uint32_t)n_in) ? in[i + 1] : a;
            int16_t s;

            if (frac == 0)
              {
                s = a;
              }
            else
              {
                s = (int16_t)(((int32_t)a * (3 - (int32_t)frac) +
                               (int32_t)b * (int32_t)frac) / 3);
              }

            outbuf[n_out++] = s;   /* L */
            outbuf[n_out++] = s;   /* R */
          }

        /* ⚠️ 2026-09-02 播放"卡成怪兽"修复：逐块 hal_i2s_write（阻塞等
         * 全部发完）→ 块间 DMA 空窗（flush 等 EOF + worker 往返 + 下块
         * 准备时间）→ 真实语音被切段（纯音听不出、人声"卡"）。改 async
         * 排队：排队时驱动已 memcpy 拷走数据，生成下一块时上一块仍在播，
         * DMA 由 ISR 无缝续链；函数退出前统一 flush。 */

        ret = hal_i2s_write_async(outbuf, (uint32_t)n_out * 2);
        if (ret < 0)
          {
            printf("[Voice] 播放失败: hal_i2s_write_async=%d (%s) "
                   "(sent=%d/%d)\n",
                   ret, strerror(-ret), sent, data_bytes);
            hal_i2s_write_flush();
            return ret;
          }

        sent += n_in * 2;

        /* 调试：第一块打印重采样输出的统计（确认播放数据是真实音频） */

        if (sent == n_in * 2)
          {
            uint32_t dnz = 0;
            int32_t dpeak = 0;
            int32_t dsum = 0;
            int dj;

            for (dj = 0; dj < n_out; dj++)
              {
                int32_t ds = outbuf[dj];
                if (ds != 0)
                  {
                    dnz++;
                  }

                dsum += ds;
                if (ds > dpeak)
                  {
                    dpeak = ds;
                  }
                else if (-ds > dpeak)
                  {
                    dpeak = -ds;
                  }
              }

            printf("[Voice] 播放数据: 输出=%d 采样 非零=%u(%u%%) 峰值=%d 均值=%ld\n",
                   n_out, dnz, (unsigned)(dnz * 100 / (uint32_t)n_out),
                   (int)dpeak, (long)(dsum / n_out));
          }
      }

  printf("[Voice] 播放完成\n");
  }

  /* 全部入队后等 DMA 发完（复用缓冲/换方向前必须 flush） */

  hal_i2s_write_flush();

  return 0;
}

/****************************************************************************
 * 公共 API：从文件流式播放 WAV（TTS 音频可达数百 KB，不整段进内存）
 *
 * 与 ai_voice_play 同逻辑（找 data chunk → 16k→24k 重采样 → hal_i2s_write），
 * 但数据从文件分块读（~1.6KB/块），内存占用恒定。
 ****************************************************************************/

int ai_voice_play_file(FAR const char *wav_path)
{
  static int16_t in[800];
  static int16_t outbuf[2400];
  uint8_t hdr[64];
  uint32_t data_off = 0;
  uint32_t data_bytes = 0;
  uint32_t sample_rate = 0;   /* 从 fmt chunk 读，TTS=24000（匹配 I2S 24k）*/
  uint32_t sent = 0;
  FILE *fp;
  int ret;

  if (wav_path == NULL)
    {
      return -EINVAL;
    }

  ai_voice_heap_warn("播放");

  fp = fopen(wav_path, "rb");
  if (fp == NULL)
    {
      return -errno;
    }

  if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) != 0)
    {
      fclose(fp);
      return -EINVAL;
    }

  /* 遍历 chunk 找 data + fmt（文件流式，只读 chunk 头 + fmt 采样率） */

  for (;;)
    {
      uint8_t ch[8];

      if (fread(ch, 1, 8, fp) != 8)
        {
          fclose(fp);
          return -EINVAL;
        }

      {
        uint32_t csize = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                         ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

        if (memcmp(ch, "fmt ", 4) == 0 && csize >= 8)
          {
            uint8_t fhdr[8];

            /* fmt 数据：audio_format(2)+channels(2)+sample_rate(4) */
            if (fread(fhdr, 1, 8, fp) == 8)
              {
                sample_rate = (uint32_t)fhdr[4] | ((uint32_t)fhdr[5] << 8) |
                              ((uint32_t)fhdr[6] << 16) |
                              ((uint32_t)fhdr[7] << 24);
              }

            fseek(fp, (long)csize - 8 + (csize & 1), SEEK_CUR);
          }
        else if (memcmp(ch, "data", 4) == 0)
          {
            data_off = (uint32_t)ftell(fp);
            data_bytes = csize;
            break;
          }
        else
          {
            fseek(fp, (long)csize + (csize & 1), SEEK_CUR);
          }
      }
    }

  /* ⚠️ 2026-08-31 修复：MiMo TTS 输出 24000Hz（check_wav 实测），
   * I2S 也跑 24kHz → 24k 直接播放（L=R 复制，无需重采样）。
   * 旧代码假设 16kHz 做 3:2 重采样 → 24k 数据被错位 → 无声/音调错。
   * 16kHz（如旧 WAV/录音）才走 3:2 插值。 */

  if (sample_rate != 24000 && sample_rate != 16000)
    {
      printf("[Voice] 播放警告: 未知采样率 %u Hz，按 24k 原样播放\n",
             (unsigned)sample_rate);
    }

  printf("[Voice] 播放 %s: %u Hz, %uB data\n",
         wav_path, (unsigned)sample_rate, (unsigned)data_bytes);

  ret = ai_voice_init();
  if (ret < 0)
    {
      fclose(fp);
      return ret;
    }

  fseek(fp, (long)data_off, SEEK_SET);

  {
    uint32_t t0 = clock_systime_ticks();

    while (sent < data_bytes)
      {
      int n_in;
      int n_out = 0;
      int j;
      size_t got;

      /* 播放中也支持取消（微信式随放随停：TTS 播放到一半再点按键 → 立即停） */

      if (g_stream_cancel)
        {
          printf("[Voice] 播放被取消（用户点按停止）\n");
          break;
        }

      n_in = (data_bytes - sent) / 2;
      if (n_in > 800)
        {
          n_in = 800;
        }

      if (n_in <= 0)
        {
          break;
        }

      got = fread(in, 2, (size_t)n_in, fp);
      if (got == 0)
        {
          break;
        }

      n_in = (int)got;

      if (sample_rate == 16000)
        {
          /* 16k → 24k：3:2 线性插值（同 ai_voice_play） */

          for (j = 0; j < n_in * 3 / 2; j++)
            {
              uint32_t pos = (uint32_t)j * 2;
              uint32_t i = pos / 3;
              uint32_t frac = pos % 3;
              int16_t a = in[i];
              int16_t b = (i + 1 < (uint32_t)n_in) ? in[i + 1] : a;
              int16_t s;

              if (frac == 0)
                {
                  s = a;
                }
              else
                {
                  s = (int16_t)(((int32_t)a * (3 - (int32_t)frac) +
                                 (int32_t)b * (int32_t)frac) / 3);
                }

              outbuf[n_out++] = s;
              outbuf[n_out++] = s;
            }
        }
      else
        {
          /* 24k（TTS）：直接 L=R 复制到 24k stereo 帧，无需重采样 */

          for (j = 0; j < n_in; j++)
            {
              outbuf[n_out++] = in[j];
              outbuf[n_out++] = in[j];
            }
        }

      /* ⚠️ 2026-09-02 播放"卡"修复：同 ai_voice_play——逐块阻塞改
       * async 排队（驱动已拷走数据 → 生成下一块时上一块在播，ISR 无缝
       * 续链），消除每 50ms 块边界的 DMA 空窗。文件读取/重采样耗时被
       * async 节流吸收，不再造成可听停顿。 */

      ret = hal_i2s_write_async(outbuf, (uint32_t)n_out * 2);
      if (ret < 0)
        {
          /* ⚠️ hal_i2s_write_async 返回真实错误码（不再折叠成 -EIO），
           * 便于区分 -ETIMEDOUT(DMA 未完成)/-ENOMEM(缓冲池) 等根因 */

          printf("[Voice] 文件播放失败: hal_i2s_write_async=%d (%s)\n",
                 ret, strerror(-ret));
          hal_i2s_write_flush();
          fclose(fp);
          return ret;
        }

      /* ⚠️ 2026-09-02 播放诊断：第一块打印实际播放数据统计——
       * 若此处非零/峰值正常但喇叭无声 → 问题在码片/功放/DMA 数据未
       * 真正串行化（查 [I2S] TX 状态 + ES8311 寄存器，plant voice regs）；
       * 若此处全零 → 问题在录音/WAV 数据（信号统计应为非零）。 */

      if (sent == 0)
        {
          uint32_t dnz = 0;
          int32_t dpeak = 0;
          int32_t dsum = 0;
          int dj;

          for (dj = 0; dj < n_out; dj++)
            {
              int32_t ds = outbuf[dj];

              if (ds != 0)
                {
                  dnz++;
                }

              dsum += ds;
              if (ds > dpeak)
                {
                  dpeak = ds;
                }
              else if (-ds > dpeak)
                {
                  dpeak = -ds;
                }
            }

          printf("[Voice] 播放数据: 输出=%d 采样 非零=%u(%u%%) 峰值=%d "
                 "均值=%ld\n",
                 n_out, dnz, (unsigned)(dnz * 100 / (uint32_t)n_out),
                 (int)dpeak, (long)(dsum / n_out));
        }

      sent += (uint32_t)n_in * 2;
    }

    /* 全部入队后等 DMA 发完（复用缓冲/换方向前必须 flush） */

    hal_i2s_write_flush();

    {
      uint32_t elapsed = clock_systime_ticks() - t0;
      uint32_t expect;

      /* 理论播放时长：data_bytes 采样 @sample_rate Hz → 秒 → ticks。
       * 96000B@16k mono = 48000/16000 = 3s = 300 ticks；24k = 2s = 200。 */

      expect = (uint32_t)(data_bytes / 2) * 100 / sample_rate;

      /* ⚠️ 播放耗时判读（10ms/tick）：实际明显 > 理论+10% → 播放仍有
       * 停顿（SD 读取/worker 延迟/块间隙）；≈ 理论 → 实时连续。 */

      printf("[Voice] 播放耗时=%u ticks (理论≈%u, %s)\n",
             elapsed, expect,
             elapsed <= expect + expect / 10 ? "实时连续" : "有停顿!");
    }
  }

  fclose(fp);
  printf("[Voice] 文件播放完成 (%s, %uB)\n", wav_path, (unsigned)data_bytes);
  return 0;
}

/****************************************************************************
 * 公共 API：流式播放（边下边播）
 *
 * 网络字节 → RIFF 解析 → I2S，全程不落盘、不 seek。
 * 全 static 缓冲（语音链路零大 malloc 的约定不变）。
 ****************************************************************************/

/* RIFF 解析状态 */

#define SP_RIFF   0
#define SP_CHUNK  1
#define SP_FMT    2
#define SP_SKIP   3
#define SP_DATA   4

/* PCM 暂存：满 6400B（=4×1600B，即 4 块 800 采样）才喂一次 I2S。
 * 它同时充当起播前预缓冲 —— 第一批数据到手就出声容易在起播瞬间
 * 断一下，攒够 200ms@16k/133ms@24k 再开声，之后网络速率（实测
 * 百 KB/s）远高于播放消耗（32~48KB/s），不会再断。 */

#define SP_PCM_STAGE 6400
#define SP_PCM_CHUNK 800      /* 每次转 I2S 的采样数（同 play_file） */

static uint8_t  s_sp_hdr[64];
static size_t   s_sp_hdr_len;
static uint32_t s_sp_skip;
static uint32_t s_sp_fmt_size;
static uint32_t s_sp_rate;
static uint32_t s_sp_data_bytes;
static uint8_t  s_sp_pcm[SP_PCM_STAGE];
static size_t   s_sp_pcm_len;
static uint32_t s_sp_written;      /* 已喂给 I2S 的 PCM 字节 */
static uint32_t s_sp_recv;         /* 收到的 PCM 字节 */
static uint32_t s_sp_t0;
static int      s_sp_st;
static bool     s_sp_active;

static uint32_t sp_rd32(FAR const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 把暂存里的 PCM 转成 I2S 帧发出去。fin=true 时收尾（丢掉末尾不够
 * 一个采样的半个字节）。返回 0 成功，<0 为 hal_i2s_write_async 错误。 */

static int sp_flush_pcm(bool fin)
{
  static int16_t in[SP_PCM_CHUNK];
  static int16_t outbuf[SP_PCM_CHUNK * 3];
  size_t avail = s_sp_pcm_len;
  size_t off = 0;
  int ret;

  (void)fin;

  avail &= ~(size_t)1;   /* 16bit 对齐：半个采样留着下一块再来 */

  while (off + 2 <= avail)
    {
      size_t n_in = (avail - off) / 2;
      size_t j;
      int n_out = 0;

      if (n_in > SP_PCM_CHUNK)
        {
          n_in = SP_PCM_CHUNK;
        }

      for (j = 0; j < n_in; j++)
        {
          in[j] = (int16_t)((uint16_t)s_sp_pcm[off + j * 2] |
                            ((uint16_t)s_sp_pcm[off + j * 2 + 1] << 8));
        }

      if (s_sp_rate == 16000)
        {
          /* 16k → 24k：3:2 线性插值（同 ai_voice_play / play_file） */

          for (j = 0; j < n_in * 3 / 2; j++)
            {
              uint32_t pos = (uint32_t)j * 2;
              uint32_t i = pos / 3;
              uint32_t frac = pos % 3;
              int16_t a = in[i];
              int16_t b = (i + 1 < (uint32_t)n_in) ? in[i + 1] : a;
              int16_t s;

              if (frac == 0)
                {
                  s = a;
                }
              else
                {
                  s = (int16_t)(((int32_t)a * (3 - (int32_t)frac) +
                                 (int32_t)b * (int32_t)frac) / 3);
                }

              outbuf[n_out++] = s;
              outbuf[n_out++] = s;
            }
        }
      else
        {
          /* 24k：直接 L=R 复制（I2S 就跑 24k，无需重采样） */

          for (j = 0; j < n_in; j++)
            {
              outbuf[n_out++] = in[j];
              outbuf[n_out++] = in[j];
            }
        }

      ret = hal_i2s_write_async(outbuf, (uint32_t)n_out * 2);
      if (ret < 0)
        {
          printf("[Voice] 流式播放失败: hal_i2s_write_async=%d (%s)\n",
                 ret, strerror(-ret));
          hal_i2s_write_flush();
          return ret;
        }

      s_sp_written += (uint32_t)n_in * 2;
      off += n_in * 2;
    }

  if (off > 0)
    {
      memmove(s_sp_pcm, s_sp_pcm + off, s_sp_pcm_len - off);
      s_sp_pcm_len -= off;
    }

  return 0;
}

static int sp_feed_pcm(FAR const uint8_t *data, size_t len)
{
  while (len > 0)
    {
      size_t room = sizeof(s_sp_pcm) - s_sp_pcm_len;
      size_t n = (len < room) ? len : room;
      int ret;

      memcpy(s_sp_pcm + s_sp_pcm_len, data, n);
      s_sp_pcm_len += n;
      s_sp_recv += (uint32_t)n;
      data += n;
      len -= n;

      if (s_sp_pcm_len >= sizeof(s_sp_pcm))
        {
          ret = sp_flush_pcm(false);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return 0;
}

/* 吃掉暂存里的 RIFF 头，直到定位到 data 区。
 * 返回 0=继续喂头，<0=音频格式不认识。 */

static int sp_parse_header(void)
{
  for (;;)
    {
      if (s_sp_st == SP_SKIP)
        {
          size_t n = (s_sp_hdr_len < (size_t)s_sp_skip)
                     ? s_sp_hdr_len : (size_t)s_sp_skip;

          if (n == 0)
            {
              return 0;
            }

          memmove(s_sp_hdr, s_sp_hdr + n, s_sp_hdr_len - n);
          s_sp_hdr_len -= n;
          s_sp_skip -= (uint32_t)n;

          if (s_sp_skip == 0)
            {
              s_sp_st = SP_CHUNK;
            }

          continue;
        }

      if (s_sp_st == SP_RIFF)
        {
          if (s_sp_hdr_len < 12)
            {
              return 0;
            }

          if (memcmp(s_sp_hdr, "RIFF", 4) != 0 ||
              memcmp(s_sp_hdr + 8, "WAVE", 4) != 0)
            {
              printf("[Voice] 流式播放: 不是 RIFF/WAVE（首4字节 "
                     "%02X %02X %02X %02X）\n",
                     s_sp_hdr[0], s_sp_hdr[1], s_sp_hdr[2], s_sp_hdr[3]);
              return -EINVAL;
            }

          memmove(s_sp_hdr, s_sp_hdr + 12, s_sp_hdr_len - 12);
          s_sp_hdr_len -= 12;
          s_sp_st = SP_CHUNK;
          continue;
        }

      if (s_sp_st == SP_CHUNK)
        {
          uint32_t csize;

          if (s_sp_hdr_len < 8)
            {
              return 0;
            }

          csize = sp_rd32(s_sp_hdr + 4);

          if (memcmp(s_sp_hdr, "data", 4) == 0)
            {
              s_sp_data_bytes = csize;
              memmove(s_sp_hdr, s_sp_hdr + 8, s_sp_hdr_len - 8);
              s_sp_hdr_len -= 8;
              s_sp_st = SP_DATA;
              return 0;
            }

          if (memcmp(s_sp_hdr, "fmt ", 4) == 0 && csize >= 8)
            {
              s_sp_fmt_size = csize;
              memmove(s_sp_hdr, s_sp_hdr + 8, s_sp_hdr_len - 8);
              s_sp_hdr_len -= 8;
              s_sp_st = SP_FMT;
              continue;
            }

          s_sp_skip = csize + (csize & 1);
          memmove(s_sp_hdr, s_sp_hdr + 8, s_sp_hdr_len - 8);
          s_sp_hdr_len -= 8;
          s_sp_st = (s_sp_skip > 0) ? SP_SKIP : SP_CHUNK;
          continue;
        }

      if (s_sp_st == SP_FMT)
        {
          if (s_sp_hdr_len < 8)
            {
              return 0;
            }

          s_sp_rate = sp_rd32(s_sp_hdr + 4);
          memmove(s_sp_hdr, s_sp_hdr + 8, s_sp_hdr_len - 8);
          s_sp_hdr_len -= 8;
          s_sp_skip = s_sp_fmt_size - 8 + (s_sp_fmt_size & 1);
          s_sp_st = (s_sp_skip > 0) ? SP_SKIP : SP_CHUNK;
          continue;
        }

      return 0;   /* SP_DATA：头解析完了 */
    }
}

int ai_voice_stream_play_begin(void)
{
  int ret;

  s_sp_active = false;
  s_sp_st = SP_RIFF;
  s_sp_hdr_len = 0;
  s_sp_skip = 0;
  s_sp_fmt_size = 0;
  s_sp_rate = 24000;         /* 拿不到 fmt 就按 24k（TTS 的实际情况） */
  s_sp_data_bytes = 0;
  s_sp_pcm_len = 0;
  s_sp_written = 0;
  s_sp_recv = 0;

  ai_voice_heap_warn("流式播放");

  ret = ai_voice_init();
  if (ret < 0)
    {
      return ret;
    }

  s_sp_t0 = clock_systime_ticks();
  s_sp_active = true;
  return 0;
}

int ai_voice_stream_play_push(FAR const uint8_t *data, size_t len)
{
  int ret;

  if (!s_sp_active)
    {
      return -EINVAL;
    }

  if (data == NULL || len == 0)
    {
      return 0;
    }

  while (len > 0)
    {
      if (g_stream_cancel)
        {
          return -ECANCELED;
        }

      if (s_sp_st == SP_DATA)
        {
          return sp_feed_pcm(data, len);
        }

      {
        size_t room = sizeof(s_sp_hdr) - s_sp_hdr_len;
        size_t n;

        if (room == 0)
          {
            printf("[Voice] 流式播放: RIFF 头异常（>%u B 仍未到 data）\n",
                   (unsigned)sizeof(s_sp_hdr));
            return -EINVAL;
          }

        n = (len < room) ? len : room;
        memcpy(s_sp_hdr + s_sp_hdr_len, data, n);
        s_sp_hdr_len += n;
        data += n;
        len -= n;
      }

      ret = sp_parse_header();
      if (ret < 0)
        {
          return ret;
        }

      if (s_sp_st == SP_DATA)
        {
          /* 头解析完成：暂存里剩的字节就是 PCM（最多 64B） */

          size_t n = s_sp_hdr_len;

          s_sp_hdr_len = 0;

          if (n > 0)
            {
              ret = sp_feed_pcm(s_sp_hdr, n);
              if (ret < 0)
                {
                  return ret;
                }
            }
        }
    }

  return 0;
}

int ai_voice_stream_play_end(void)
{
  uint32_t elapsed;
  uint32_t expect = 0;
  int ret = 0;

  if (!s_sp_active)
    {
      return -EINVAL;
    }

  if (s_sp_pcm_len > 0 && !g_stream_cancel)
    {
      ret = sp_flush_pcm(true);
    }

  s_sp_pcm_len = 0;
  s_sp_hdr_len = 0;
  s_sp_active = false;
  s_sp_st = SP_RIFF;

  hal_i2s_write_flush();

  elapsed = clock_systime_ticks() - s_sp_t0;

  if (s_sp_rate > 0)
    {
      /* 理论播放时长：PCM 字节 /2/采样率 秒 → ticks(10ms) */

      expect = (uint32_t)(((uint64_t)s_sp_written * 50) / s_sp_rate);
    }

  printf("[Voice] 流式播放结束: 收到 %uB 已播 %uB @%uHz 耗时=%u ticks "
         "(理论≈%u, %s)\n",
         (unsigned)s_sp_recv, (unsigned)s_sp_written, (unsigned)s_sp_rate,
         elapsed, expect,
         (expect == 0 || elapsed <= expect + expect / 10)
           ? "实时连续" : "有停顿!");

  return ret;
}

int ai_voice_stream_cancel_requested(void)
{
  return g_stream_cancel ? 1 : 0;
}

/****************************************************************************
 * 公共 API：播放测试音（正弦波）
 ****************************************************************************/

int ai_voice_play_tone(uint32_t freq_hz, int ms)
{
  /* ⚠️ 2026-09-02 WROOM 内存修复：旧实现 malloc(48KB) 在 512KB SRAM
   * 堆耗尽时失败（实测 tone ret=-12 → 无声假象）。改【小块 static + 分块发送】：
   * 50ms/块 = 4.8KB（比 100ms/块再省 4.8KB），与录音/播放链路同策略。 */

  /* ⚠️ 2026-09-02 v2（"吱吱吱嘟嘟"修复）：分块发送改【异步排队 + 末尾
   * flush】——hal_i2s_write_async 排队即回（驱动已拷走数据，可复用本
   * 缓冲），生成下一块时上一块仍在播；块间由官方驱动 ISR 无缝续链，
   * 消除"等完再发"造成的 50ms 块边界 DMA 空窗（纯音被切段 → 嘟嘟）。
   * 旧 hal_i2s_write（阻塞等完）对纯音必现块间掉拍。 */

  static int16_t tone[1200 * 2];   /* 50ms × 24k × 2ch = 4.8KB */
  uint32_t n_total;
  uint32_t sent_samp = 0;
  int ret;

  ret = ai_voice_init();
  if (ret < 0)
    {
      return ret;
    }

  if (ms <= 0 || ms > 1000)
    {
      ms = 500;
    }

  n_total = (uint32_t)ms * AI_VOICE_SAMPLE_RATE / 1000;

  while (sent_samp < n_total)
    {
      uint32_t n = n_total - sent_samp;
      uint32_t i;

      if (n > 1200)
        {
          n = 1200;
        }

      for (i = 0; i < n; i++)
        {
          double t = (double)(sent_samp + i) / AI_VOICE_SAMPLE_RATE;
          int16_t sample =
              (int16_t)(30000.0 * sin(2.0 * 3.14159265358979 * freq_hz * t));

          tone[i * 2]     = sample;  /* Left */
          tone[i * 2 + 1] = sample;  /* Right */
        }

      ret = hal_i2s_write_async(tone, n * 2 * 2);
      if (ret < 0)
        {
          hal_i2s_write_flush();
          return ret;
        }

      sent_samp += n;
    }

  /* 全部入队后等 DMA 发完（复用 tone 缓冲 / 换方向前必须 flush） */

  ret = hal_i2s_write_flush();
  if (ret < 0)
    {
      return ret;
    }

  return 0;
}

/****************************************************************************
 * 诊断：I2C 通信测试（验证码片是否在总线上）
 ****************************************************************************/

int ai_voice_i2c_test(void)
{
  FAR struct i2c_master_s *i2c;
  uint8_t v = 0;
  int ret;
  int pass = 0;

  i2c = esp32s3_i2cbus_initialize(I2C_BUS);
  if (i2c == NULL)
    {
      printf("[I2C] 总线初始化失败\n");
      return -ENODEV;
    }

  printf("[I2C] === 音频码片 I2C 应答检测 (bus %d) ===\n", I2C_BUS);

  /* ES8311 (0x18): REG01 = 时钟管理/芯片 ID 区 */

  v = 0;
  ret = hal_i2c_read_reg(i2c, 0x18, 0x01, &v, 100000);
  printf("[I2C] ES8311(0x18) REG01=0x%02x (ret=%d)  %s\n",
         v, ret, (ret == 0 && v != 0xff) ? "✓ 有应答" : "✗ 无应答");
  if (ret == 0 && v != 0xff)
    {
      pass++;
    }

  /* ES8311 (0x18): REG00 = 复位/主从 */

  v = 0;
  ret = hal_i2c_read_reg(i2c, 0x18, 0x00, &v, 100000);
  printf("[I2C] ES8311(0x18) REG00=0x%02x (ret=%d)  %s\n",
         v, ret, (ret == 0 && v != 0xff) ? "✓ 有应答" : "✗ 无应答");
  if (ret == 0 && v != 0xff)
    {
      pass++;
    }

  /* ES7210 (0x40): REG00 = 复位/上电默认 0x41 */

  v = 0;
  ret = hal_i2c_read_reg(i2c, 0x40, 0x00, &v, 100000);
  printf("[I2C] ES7210(0x40) REG00=0x%02x (ret=%d)  %s\n",
         v, ret, (ret == 0 && v != 0xff) ? "✓ 有应答" : "✗ 无应答");
  if (ret == 0 && v != 0xff)
    {
      pass++;
    }

  /* ES7210 (0x40): REG01 = 芯片 ID */

  v = 0;
  ret = hal_i2c_read_reg(i2c, 0x40, 0x01, &v, 100000);
  printf("[I2C] ES7210(0x40) REG01=0x%02x (ret=%d)  %s\n",
         v, ret, (ret == 0 && v != 0xff) ? "✓ 有应答" : "✗ 无应答");

  printf("[I2C] 结果: %d/3 次读操作有应答\n", pass);
  return pass >= 2 ? 0 : -ENODEV;
}
