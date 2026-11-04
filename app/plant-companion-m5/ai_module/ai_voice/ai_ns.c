/****************************************************************************
 * ai_ns.c — esp_sr NSNet2 神经网络降噪封装（方案A：xiaozhi 同款 nsnet）
 *
 * 背景：xiaozhi 用 ESP-SR 的 NSNet2（esp_nn 优化算子，S3 实时）。
 * RNNoise 纯 C GRU 实测在 ESP32-S3 上卡死（无 SIMD，浮点太慢）。
 *
 * 本文件用 esp_sr 预编译库（libnsnet.a + libdl_lib.a + libhufzip.a）
 * 的 NSNet 接口（esp_nsn_iface_t）：
 *   esp_nsnet_handle_from_name("nsnet2") → create → process
 *
 * 模型：nsnet2（337KB，srmodels.bin 打包）以 const 数组嵌入固件
 * （esp_sr/model_data/srmodels_data.c），srmodel_shim.c 内存解析，
 * model_create 直接从固件内嵌数据加载到 PSRAM —— 无需文件系统。
 *
 * RAM 规划：nsnet 模型权重 + 中间张量由 dl_lib 经 heap_caps 分配
 *（CONFIG_ESP32S3_SPIRAM_COMMON_HEAP 启用时大块落 PSRAM），内部
 * RAM 只留帧缓冲。
 *
 * 接口（与旧 RNNoise 版一致，调用方不变）：
 *   ai_ns_init()          创建 nsnet2 实例（16kHz）
 *   ai_ns_process_frame() 处理一帧（ai_ns_frame_size() 个采样，int16 in-place）
 *   ai_ns_frame_size()    返回帧大小（NSNet2 每帧采样数）
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../components/voice_agent/esp_sr/include/esp_nsn_iface.h"
#include "../../components/voice_agent/esp_sr/include/esp_nsn_models.h"
#include "../../components/voice_agent/esp_sr/srmodel_shim.h"

/* nsnet 实例（跨块保持） */
static esp_nsn_iface_t *g_nsn = NULL;
static esp_nsn_data_t *g_nsn_data = NULL;
static int g_frame_size = 0;
static int g_samp_rate = 0;

/* 前置声明（ai_ns_init 中的 warmup 推理测试用） */
int ai_ns_process_frame(int16_t *samples);

/****************************************************************************
 * 公共 API
 ****************************************************************************/

/* 初始化：加载模型注册表 + 创建 nsnet2 实例，返回 0 成功 */
int ai_ns_init(void)
{
  /* 静音 esp_sr 内部 ESP_LOGI（model_create 加载模型打几十行日志，
   * USB-Serial-JTAG FIFO 满时 printf 阻塞主线程 → 假卡死） */
  esp_sr_log_silence();

  int ret = srmodel_nuttx_init();

  if (ret < 0)
    {
      return -1;
    }

  if (g_nsn == NULL)
    {
      g_nsn = esp_nsnet_handle_from_name("nsnet2");
      if (g_nsn == NULL)
        {
          printf("ai_ns: no nsnet2 handle\n");
          return -1;
        }

      printf("ai_ns: handle=%p create=%p process=%p chunksz=%p\n",
             (void *)g_nsn, (void *)g_nsn->create,
             (void *)g_nsn->process, (void *)g_nsn->get_samp_chunksize);
    }

  if (g_nsn_data == NULL)
    {
      printf("ai_ns: calling nsnet2 create...\n");
      g_nsn_data = g_nsn->create("nsnet2");
      printf("ai_ns: create returned %p\n", (void *)g_nsn_data);
      if (g_nsn_data == NULL)
        {
          printf("ai_ns: nsnet2 create failed\n");
          return -1;
        }

      printf("ai_ns: get_samp_chunksize...\n");
      g_frame_size = g_nsn->get_samp_chunksize(g_nsn_data);
      printf("ai_ns: chunksize=%d\n", g_frame_size);

      printf("ai_ns: get_samp_rate...\n");
      g_samp_rate = g_nsn->get_samp_rate(g_nsn_data);
      printf("ai_ns: samp_rate=%d\n", g_samp_rate);
    }

  return 0;
}

/* 处理一帧（g_frame_size 个 int16 采样，in-place），返回 0 成功 */
int ai_ns_process_frame(int16_t *samples)
{
  static int16_t out[1024];
  static int g_ns_call = 0;

  if (g_nsn == NULL || g_nsn_data == NULL)
    {
      return -1;
    }

  if (g_ns_call < 4)
    {
      printf("ai_ns: process #%d start tick=%lu\n", g_ns_call,
             (unsigned long)clock_systime_ticks());
    }

  g_nsn->process(g_nsn_data, samples, out);

  if (g_ns_call < 4)
    {
      printf("ai_ns: process #%d done  tick=%lu\n", g_ns_call,
             (unsigned long)clock_systime_ticks());
    }

  g_ns_call++;
  memcpy(samples, out, g_frame_size * 2);

  return 0;
}

/* 返回每帧采样数（NSNet2） */
int ai_ns_frame_size(void)
{
  return g_frame_size;
}

/* 返回采样率（16000） */
int ai_ns_samp_rate(void)
{
  return g_samp_rate;
}
