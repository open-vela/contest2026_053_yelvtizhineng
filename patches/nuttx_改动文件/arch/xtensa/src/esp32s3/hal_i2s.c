/****************************************************************************
 * hal_i2s.c — 2026-08-25 重构：从"手写寄存器直写"迁移为
 * NuttX 官方 esp32s3_i2s 驱动的封装层（保持 hal_i2s.h API 不变）。
 *
 * 背景（为什么迁移）：
 *   手写寄存器版把驱动框架自动处理的几件事做错/丢掉：
 *     1) RX_RESET 清掉时钟域帧配置 → RX 48k 错位噪声（"说话不说话一样"）
 *     2) DMA 完成信号用错（I2S TX_DONE=FIFO 空 假完成）→ 播放截断"都的一声"
 *     3) GDMA OUT/IN 共用 CPU 中断线 → 中断风暴卡死
 *     4) 神经网络级降噪缺失 → 手工滤波对付不了 ES7210 宽带底噪
 *   官方驱动（esp32s3_i2s.c）完整实现：时钟自动分频、DMA/EOF 中断、
 *   全双工 SIG_LOOPBACK、配置重放，与 IDF 例程同架构。
 *
 * ⚠️ 2026-09-02 设计定案（最省内存，勿改回流水线/双缓冲/环形队列）：
 *   曾试验"RX 流水线（2 apb 在途仿 TX 连续传输）"——引入复杂度与
 *   新 bug（预提交/补提交区间重叠、死等、回调竞态），且实测录音
 *   提前退出。根因复盘：diag 说话段"非零率恒定 69%"并非 RX 丢数据，
 *   而是 ai_voice.c 的 s_out[1600] 越界（每 4 块写 [1422..1896) 超 1600）
 *   写穿相邻 s_chunk48 + 回调越界读的**统计污染**。修复越界后回退到
 *   官方驱动原生"单 apb 串行"即够——零额外内存、零并发复杂度。
 *   前提：hal_i2s_read_slot 单次调用内部循环收满（块间处理 < I2S RX
 *   FIFO 深度（64B≈0.67ms）不溢出；验证见 diag 非零率 ≥95%）。
 *
 * ⚠️ 2026-09-02 TX/RX 切换（三策略融合定案，修 plant voice loop 失败）：
 *   【现象】录音（预读链在跑）后播放：第一块 hal_i2s_write 就失败，
 *   打印 hal_i2s_write=-5。旧修复只置 s_rx_prefetch_paused 标志——
 *   在途 RX apb（≤4092B≈43ms@24k 立体声）仍占着 RX DMA，TX 与 RX
 *   并发启动即失败（官方驱动单次 DMA 语义下的实测行为）。
 *   【融合】①小智策略：严格的通道生命周期——先停 RX 再发 TX
 *   （等价 i2s_channel_disable(rx) → write(tx)）；②OpenVela 策略：
 *   用官方驱动原语 + 真实 errno（hal_i2s_write 不再把错误折叠成 -EIO，
 *   失败带 I2S 寄存器现场）；③植小伴策略：保留环形 FIFO 预读链保证
 *   录音连续（链在 TX 前同步停、下次 read_slot 自动重启）。
 *   实现：hal_i2s_rx_prefetch_stop() 置标志后【同步等 RX DMA 空闲】
 *   （轮询在途计数 s_rx_prefetch_inflight 归零，≤300ms 兜底），再发 TX。
 *
 * 本封装保持 hal_i2s.h API 不变（voice_agent/ai_voice 无需改调用）：
 *   - TX：官方驱动 2 槽立体声（ES8311 DAC 原生匹配）
 *   - RX：官方驱动 2 槽立体声（ES7210 2 麦标准模式，LRCK 低=左/MIC1），
 *         hal_i2s_read/read_slot 取单声道（slot 0=左 MIC1 / 1=右 MIC2）
 *   - 采样率 24kHz / 16bit（Kconfig: CONFIG_ESP32S3_I2S0_SAMPLE_RATE=24000）
 *   - MCLK=GPIO2 BCLK=GPIO17 WS=GPIO45 DOUT=GPIO15 DIN=GPIO16 PA=GPIO46
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/clock.h>
#include <nuttx/semaphore.h>
#include <nuttx/signal.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <sys/param.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "xtensa.h"
#include "esp32s3_gpio.h"
#include "hardware/esp32s3_i2s.h"
#include "hal_i2s.h"

/* 官方驱动总线初始化（链接时解析） */

extern FAR struct i2s_dev_s *esp32s3_i2sbus_initialize(int port);

/* 采样参数（与 hal_i2s.h 一致；Kconfig 必须配 24000/16bit/2ch） */

#define HAL_RATE        24000
#define HAL_BITS        16
#define HAL_FRAME_BYTES 4   /* 立体声 2 槽 × 16bit */

static FAR struct i2s_dev_s *g_dev;
static sem_t g_tx_done_sem;   /* TX 每块完成 post 一次（窗口化流水线计数） */
static int g_tx_result;
static int g_rx_result;

/* ⚠️ 2026-09-02 TX 流水线：官方驱动单次 i2s_send ≤ 2044B，若逐块
 * "排队→等完成→再排队"，块间 DMA 空转（HPWORK worker 往返 > FIFO
 * 余量）→ 播放断续（纯音"吱吱吱嘟嘟"）。改为窗口内连续排队，EOF 后
 * 官方驱动 ISR 直接续链 pend 队列 → DMA 无缝。 */

#define HAL_TX_MAX_INFLIGHT   3   /* 在途 i2s_send 块上限（驱动 container 池=4，留 1 裕量） */

static int s_tx_inflight = 0;     /* 已排队未完成块数（单 TX 用户，应用线程独占） */

/* ════════════════════════════════════════════════════════════════════
 * RX 环形 FIFO + 预读自动续（2026-09-01 数据完整性修复）
 *
 * 背景：NuttX 官方 esp32s3_i2s.c 的 RX 是"单次传输"（描述符非循环，
 * 每个 apb 收满即停）。原方案"单次预读缓冲（1023 采样/42.6ms）+ read_slot
 * 手动续"在 read_slot 暂停期间（语音块回调/HTTP 上传/WiFi 抖动）DMA 停
 * → FIFO(64B≈0.67ms) 溢出丢数据 → 语音断续缺失（用户实测"听不到人声"）。
 *
 * 修复（对齐小智 IDF 循环 DMA + auto_clear_after_cb 思路的 NuttX 等效）：
 *   1. 环形 FIFO 2048 采样（4KB static）：RX 数据持续入队，read_slot 按需
 *      取走；FIFO 满则覆盖最旧（等效 auto_clear，无残留混叠）。
 *   2. 预读自动续：预读 apb 完成回调里【立即再提交下一轮】→ DMA 连续收，
 *      read_slot/回调/上传期间不再停 → 无 FIFO 溢出丢数据。
 *   3. 回调（worker）写 FIFO、read_slot（调用线程）读，临界区保护指针。
 *
 * 内存代价：+4KB static（原 2KB 预读缓冲删除，净 +2KB .bss）。
 * ════════════════════════════════════════════════════════════════════ */

#define HAL_RX_FIFO_SAMPLES  2048                 /* 环形 FIFO 采样数（4KB） */
#define HAL_RX_FIFO_MASK     (HAL_RX_FIFO_SAMPLES - 1)

/* ⚠️ 2026-09-02 RX 双深预读（"录音一卡一卡"根因修复）：
 * 硬件 RX FIFO 仅 64B≈0.67ms。单深预读 = 收完 1 个 apb（21ms）才由
 * worker 提交下一个 → apb 间 DMA 空窗（worker 往返 >0.67ms）→ 周期性
 * 丢采样（录音墙钟 ≈2× 标称，回放一卡一卡）。深度 2：预读链始终保持
 * 2 个 apb 在途（1 个 act 在收 + 1 个 pend 排队）→ apb EOF 中断（ISR，
 * 微秒级）直接从 pend 装载下一个 → 无缝。硬件 GDMA 单通道串行 + 驱动
 * rx.act 非空保护 → 两 apb 绝不重叠收数。内存增量仅 +1 apb（~4KB 瞬时
 * 堆，段长/ring FIFO 不变）。 */

/* ⚠️ 2026-09-16 定案：深度必须是 2，不能再加！
 * 试过 6 → 板子直接卡死（plant voice raw/rec 全部无输出）。原因在官方驱动：
 *   1) 驱动自带容器池只有 CONFIG_ESP32S3_I2S_MAXINFLIGHT=4 个
 *      （esp32s3_i2s.c i2s_buf_initialize），且 i2s_buf_allocate() 是
 *      nxsem_wait_uninterruptible —— 池空就永久阻塞；
 *   2) esp32s3_i2s.c 的 RX 完成判定也按"1 在收 + 1 排队"设计。
 * 想要更深的链路，必须同时改驱动完成逻辑并放大容器池，风险高。
 * 结论：深度 2，靠"应用侧不要把慢 I/O 放进采集回调"来保证不丢采样。 */
#define HAL_RX_PREFETCH_DEPTH  2    /* 预读在途 apb 深度（A/B 乒乓） */

static int16_t s_rx_fifo[HAL_RX_FIFO_SAMPLES];    /* 环形缓冲（static 4KB） */
static volatile uint32_t s_rx_fifo_rd;            /* 读指针 */
static volatile uint32_t s_rx_fifo_cnt;           /* 有效采样数 */

/* ⚠️ 2026-09-16 丢采样计数（录音质量回归指标）：RX 环形 FIFO 满时覆盖
 * 最旧样本，等于丢掉了采样 → 时间轴被压缩（语音变快/不完整/听不清）。
 * 采集线程被慢 IO 挤掉 CPU 时这个计数会快速增长。0 = 一个没丢。 */

static volatile uint32_t s_rx_fifo_drop;
static sem_t   s_rx_prefetch_sem;                 /* 有数据通知 */
static struct  hal_i2s_rx_ctx s_rx_prefetch_ctx;  /* 预读槽位上下文（共享 slot） */
static volatile bool s_rx_prefetch_paused;        /* TX 播放时暂停预读链 */

/* ⚠️ 2026-09-02：在途 RX apb 计数（深度 2 后取代单布尔 active）——
 * 目标 ≤ HAL_RX_PREFETCH_DEPTH；TX 前同步等待用计数归零判定。
 * 增/减均在临界区（read_slot 应用线程与 worker 回调并发）。 */

static volatile uint32_t s_rx_prefetch_inflight;  /* 在途 RX apb 数 */

/* ⚠️ 2026-09-16 静态 RX apb 池（根治"偶发堆损坏 → 板卡挂死"）
 *
 * 实测根因：官方 apb_alloc() 把 apb 结构与采样缓冲【分两次】从 UMM 堆
 * 分配，而本板 UMM 堆含 PSRAM 区（CONFIG_MM_REGIONS=2）。WiFi 关联等
 * 堆抖动时刻，两者会落到外部内存（实测 apb=0x3c1c2018），此时 DMA 目标
 * 不再位于内部 SRAM；且录音期间每 ~21ms 产生一对 alloc/free（≈90 次堆
 * 操作/秒）使堆长期零碎。现场后果（两者都实测到）：
 *   ① hpwork 里 apb_free → free → mm_malloc_size.c:78 断言
 *   ② 回调读 apb->samp（hal_i2s.c:217）触发 LoadProhibited，VADDR 为垃圾
 * 修法：初始化时一次性建池，采样缓冲用本文件静态缓冲（必然内部 SRAM、
 * 32 字节对齐 = cache line），并给每个 apb 保留一个"永久引用"使
 * apb_free 的引用计数永不降到 1 → 运行期零分配、零释放、DMA 恒可达。
 *
 * 引用计数推演（每轮）：本函数 +1、i2s_receive +1、完成回调 -1、
 * 官方驱动 -1 → 每轮净 0，计数恒 ≥2。 */

#define HAL_RX_POOL_N       4      /* 槽数必须 > 预读深度（2） */
#define HAL_RX_SAMP_N       1024   /* 2044B 上限 = 1022 采样，取 1024 */
#define HAL_RX_SAMP_BYTES   (HAL_RX_SAMP_N * 2)

static int16_t s_rx_pool_samp[HAL_RX_POOL_N][HAL_RX_SAMP_N]
              __attribute__((aligned(32)));
static FAR struct ap_buffer_s *s_rx_pool_apb[HAL_RX_POOL_N];
static bool  s_rx_pool_ready;      /* 池已建好 */
static int   s_rx_pool_next;       /* 轮转下标 */

static int hal_i2s_rx_pool_init(void)
{
  struct audio_buf_desc_s desc;
  int i;

  if (s_rx_pool_ready)
    {
      return 0;
    }

  for (i = 0; i < HAL_RX_POOL_N; i++)
    {
      s_rx_pool_apb[i] = NULL;
      memset(&desc, 0, sizeof(desc));
      desc.numbytes  = 32;              /* 占位，真缓冲用 s_rx_pool_samp */
      desc.u.pbuffer = &s_rx_pool_apb[i];

      if (apb_alloc(&desc) < 0 || s_rx_pool_apb[i] == NULL)
        {
          printf("[I2S] RX apb 池分配失败(槽 %d)\n", i);
          return -ENOMEM;
        }

      apb_reference(s_rx_pool_apb[i]);  /* 永久引用（见上方推演） */
      s_rx_pool_apb[i]->samp = (FAR uint8_t *)s_rx_pool_samp[i];
    }

  s_rx_pool_ready = true;
  return 0;
}

static int hal_i2s_rx_prefetch_submit(int slot);
static void hal_i2s_rx_prefetch_start(int slot);


/****************************************************************************
 * 内部：DMA 完成回调（官方驱动异步 API 的同步包装）
 *
 * ⚠️ 2026-08-25 apb 引用竞态修复（UAF → 堆损坏/泄漏）：
 * 官方驱动完成路径 = callback() → apb_free(apb)（释放驱动引用）。
 * 若调用者在 sem 唤醒后才 apb_free，可能抢在官方驱动前面把 crefs
 * 降到 0 释放内存 → 官方驱动随后 apb_free 访问已释放内存（UAF）。
 * 修复：在回调内 apb_free（我们的引用 2→1），官方驱动随后 1→0
 * 最终释放——释放顺序全在 worker 线程内，无竞态；数据提取也移进
 * 回调（回调返回后不再访问 apb）。
 ****************************************************************************/

struct hal_i2s_rx_ctx
{
  FAR int16_t *dst;   /* 目标单声道缓冲 */
  int           slot; /* 0=左 MIC1 / 1=右 MIC2 */
  uint32_t      got;  /* 已提取采样数 */
};

static void hal_i2s_tx_cb(FAR struct i2s_dev_s *dev,
                          FAR struct ap_buffer_s *apb,
                          FAR void *arg, int result)
{
  g_tx_result = result;

  /* 释放我们的引用；官方驱动随后再释放（1→0）完成最终释放 */

  apb_free(apb);
  sem_post(&g_tx_done_sem);
}

/* 预读 apb 完成回调（worker 线程）：环形写 FIFO + post + 自动续（DMA 连续） */

static void hal_i2s_rx_prefetch_cb(FAR struct i2s_dev_s *dev,
                                   FAR struct ap_buffer_s *apb,
                                   FAR void *arg, int result)
{
  FAR struct hal_i2s_rx_ctx *ctx = (FAR struct hal_i2s_rx_ctx *)arg;
  uint32_t nframe = (uint32_t)(apb->nbytes / 2);
  FAR const int16_t *src = (FAR const int16_t *)apb->samp;
  uint32_t i;
  irqstate_t flags;

  (void)dev;
  g_rx_result = result;

  /* 在途计数 -1（临界区：与 read_slot 的 submit 预订并发）。
   * 本 apb 的 DMA 已完成；TX 前同步等待据此判空闲。 */

  flags = enter_critical_section();
  if (s_rx_prefetch_inflight > 0)
    {
      s_rx_prefetch_inflight--;
    }

  leave_critical_section(flags);

  apb_free(apb);                  /* 先释放，apb 池空出 */

  /* 环形写 FIFO（满则覆盖最旧 = auto_clear 等效，无残留混叠）
   * ⚠️ 2026-09-08 修复：写位置必须是环形尾 rd+cnt；旧写法 wr+cnt
   * （wr 从不更新，恒等于在绝对下标 cnt 处写），当 rd>0 时会周期性
   * 读到未写入桶位（static 初始化 0）→ 每 ~61ms 一段零数据，
   * 录音可闻 "嘘嘘" 底噪。 */
  flags = enter_critical_section();
  for (i = 0; i < nframe / 2; i++)
    {
      uint32_t w = (s_rx_fifo_rd + s_rx_fifo_cnt) & HAL_RX_FIFO_MASK;

      s_rx_fifo[w] = src[2 * i + ctx->slot];
      if (s_rx_fifo_cnt < HAL_RX_FIFO_SAMPLES)
        {
          s_rx_fifo_cnt++;
        }
      else
        {
          s_rx_fifo_rd = (s_rx_fifo_rd + 1) & HAL_RX_FIFO_MASK;
          s_rx_fifo_drop++;      /* FIFO 满 → 覆盖最旧 = 丢采样（计数） */
        }
    }

  leave_critical_section(flags);
  sem_post(&s_rx_prefetch_sem);   /* 通知 read_slot 有数据 */

  /* ⚠️ 自动续到深度 2：本 apb 完成 → 补一个进 pend，让队列恒有 ≥1 个
   * 在等 → 下一个 apb 的 EOF 中断（ISR）直接装载，无 worker 空窗。
   * 只有未暂停（播放中）才续——否则 stop 无效、TX 仍与 RX 并发。 */

  if (!s_rx_prefetch_paused)
    {
      hal_i2s_rx_prefetch_submit(ctx->slot);
    }
}

/* 提交一个预读 RX apb（不等待）。窗口（≤ HAL_RX_PREFETCH_DEPTH）用
 * 临界区"检查+预订"保证——read_slot（应用线程）与完成回调（worker）
 * 可能并发调用，防止超深/重复提交。 */

static int hal_i2s_rx_prefetch_submit(int slot)
{
  FAR struct ap_buffer_s *apb;
  uint32_t want;
  int idx;
  int ret = -ENOMEM;
  irqstate_t flags;

  /* 窗口检查 + 预订（临界区） */

  flags = enter_critical_section();
  if (s_rx_prefetch_paused || s_rx_prefetch_inflight >= HAL_RX_PREFETCH_DEPTH)
    {
      leave_critical_section(flags);
      return 0;   /* 已暂停 / 窗口已满 */
    }

  s_rx_prefetch_inflight++;       /* 预订：本 apb 将入队 */
  leave_critical_section(flags);

  want = HAL_RX_FIFO_SAMPLES * 2;   /* 立体声 2 槽 × 16bit = 单声道采样×4 */
  if (want > HAL_RX_SAMP_BYTES)
    {
      want = HAL_RX_SAMP_BYTES;     /* 不得超静态缓冲 */
    }

  if (want > 2044)
    {
      /* ⚠️ 2026-09-02 内存适配：原 4092B apb 在系统堆见底时
       * （free≈3.2KB/largest≈3.2KB，LVGL 80KB + 相机 37.5KB 静态挤压）
       * apb_alloc 失败 → 预读链不启动 → RX_START=0 → 0 数据。
       * 降 2044B（511 帧≈21ms/轮）保证 apb 总能从堆分配成功。
       * 双深后两个 apb 同时在途 ≈ +4KB 瞬时堆（堆空闲 22KB，无压力）。 */

      want = 2044;
    }

  if (hal_i2s_rx_pool_init() < 0)
    {
      goto errout_with_resv;
    }

  idx = s_rx_pool_next;
  s_rx_pool_next = (s_rx_pool_next + 1) % HAL_RX_POOL_N;
  apb = s_rx_pool_apb[idx];

  s_rx_prefetch_ctx.slot = slot;
  s_rx_prefetch_ctx.got = 0;

  apb_reference(apb);          /* 本周期 +1（池内 apb 永不真正释放） */
  apb->samp      = (FAR uint8_t *)s_rx_pool_samp[idx];
  apb->nmaxbytes = want;
  apb->nbytes    = want;
  apb->curbyte   = 0;

  ret = g_dev->ops->i2s_receive(g_dev, apb, hal_i2s_rx_prefetch_cb,
                                &s_rx_prefetch_ctx, SEC2TICK(3));
  if (ret < 0)
    {
      apb_free(apb);           /* 还掉本周期加的引用 */
      goto errout_with_resv;
    }

  return 0;

errout_with_resv:
  /* 入队失败：回滚预订（下次 read_slot/回调再试） */

  flags = enter_critical_section();
  if (s_rx_prefetch_inflight > 0)
    {
      s_rx_prefetch_inflight--;
    }

  leave_critical_section(flags);
  return ret;
}

/* 启动/补满预读链到深度 2（幂等：已在跑则 submit 因窗口满自动跳过）。
 * read_slot 每次进入都调用，保证链在跑且满。 */

static void hal_i2s_rx_prefetch_start(int slot)
{
  s_rx_prefetch_paused = false;
  s_rx_prefetch_ctx.slot = slot;   /* 链启动时固定槽位（整段录音期不变） */

  while (s_rx_prefetch_inflight < HAL_RX_PREFETCH_DEPTH)
    {
      if (hal_i2s_rx_prefetch_submit(slot) != 0)
        {
          break;   /* 分配失败等：read_slot 下次进入再补 */
        }
    }
}

/* 暂停预读链（TX 播放时调用）：在途 apb 完成后回调不再续链 →
 * RX DMA 停止、apb 池释放给 TX。read_slot 下次调用自动重启。
 *
 * ⚠️ 2026-09-02 三策略融合修复（plant voice loop 播放 -5 根因）：
 * 仅置 paused 不够——【在途 RX apb 仍占着 RX DMA】，此时启动 TX 实测
 * 失败。必须同步等 RX DMA 真正空闲（在途计数归零）再返回：
 *   ① 小智策略：严格通道生命周期，先停 RX 再发 TX（等价
 *      i2s_channel_disable(rx) → i2s_channel_write(tx)）；
 *   ② OpenVela 策略：官方驱动单次 DMA，回调即传输完成；
 *   ③ 植小伴策略：预读链保持录音连续，此处只在 TX 前同步停。
 * 竞态边界：极端情况下两个预读 apb 可能同时在途（active 标志会在
 * 第一个完成时就清 0），故用 s_rx_prefetch_inflight 计数判定——每个
 * apb 入队 +1、回调完成 -1，归零 = 全部 RX DMA 完成。300ms 兜底防
 * RX 死链导致无限等。 */

static void hal_i2s_rx_prefetch_stop(void)
{
  int wait_ms = 0;

  /* ⚠️ 置 paused：回调看到后不再续链；在途 apb 完成后链自然停止。
   * 注意：不能清 active/inflight——它们是"在途 RX apb"的真相，
   * 清掉会让下面的等待失效；回调完成时自己会更新。 */

  s_rx_prefetch_paused = true;

  /* 同步等 RX DMA 空闲：在途计数归零（所有 RX apb 的 DMA 已完成）
   * 前不发 TX */

  while (s_rx_prefetch_inflight > 0 && wait_ms < 300)
    {
      nxsig_usleep(1000);
      wait_ms++;
    }
}

/****************************************************************************
 * 公共 API
 ****************************************************************************/

int hal_i2s_init(void)
{
  int ret;

  if (g_dev != NULL)
    {
      return 0;
    }

  sem_init(&g_tx_done_sem, 0, 0);
  sem_init(&s_rx_prefetch_sem, 0, 0);
  s_rx_prefetch_paused = false;
  s_rx_prefetch_inflight = 0;

  /* PA 使能（GPIO46，高电平 = 喇叭功放开） */

  esp32s3_configgpio(HAL_PA_PIN, OUTPUT);
  esp32s3_gpiowrite(HAL_PA_PIN, true);

  /* 官方驱动初始化（按 Kconfig：master、24k、16bit、2 槽全双工；
   * 内部自动配时钟/DMA/中断，TX+RX 通道已启动） */

  g_dev = esp32s3_i2sbus_initialize(0);
  if (g_dev == NULL)
    {
      return -ENODEV;
    }

  /* 显式确认运行时参数（与 Kconfig 一致，防默认值漂移） */

  if (g_dev->ops->i2s_txsamplerate != NULL)
    {
      g_dev->ops->i2s_txsamplerate(g_dev, HAL_RATE);
    }

  if (g_dev->ops->i2s_rxsamplerate != NULL)
    {
      g_dev->ops->i2s_rxsamplerate(g_dev, HAL_RATE);
    }

  if (g_dev->ops->i2s_txdatawidth != NULL)
    {
      g_dev->ops->i2s_txdatawidth(g_dev, HAL_BITS);
    }

  if (g_dev->ops->i2s_rxdatawidth != NULL)
    {
      g_dev->ops->i2s_rxdatawidth(g_dev, HAL_BITS);
    }

  if (g_dev->ops->i2s_txchannels != NULL)
    {
      g_dev->ops->i2s_txchannels(g_dev, 2);
    }

  if (g_dev->ops->i2s_rxchannels != NULL)
    {
      g_dev->ops->i2s_rxchannels(g_dev, 2);
    }

  /* ⚠️ 2026-08-25：TX FIFO 空时也要保持 BCK/WS 输出（STOP_EN=0）。
   * 官方驱动未配置该位——录音只开 RX 时 TX 无数据流，若 FIFO 空时钟
   * 停 → RX（SIG_LOOPBACK 共享 TX 时钟）收不到数据。 */

  modifyreg32(I2S_TX_CONF_REG(0), I2S_TX_STOP_EN, 0);
  modifyreg32(I2S_TX_CONF_REG(0), 0, I2S_TX_UPDATE);
  {
    int sync_wait = 0;

    while ((getreg32(I2S_TX_CONF_REG(0)) & I2S_TX_UPDATE) &&
           sync_wait++ < 100000)
      {
      }
  }

  /* ⚠️ 2026-08-25 根因修复：TX_START 必须显式置位！
   * 官方驱动只在 TX 数据排队时置 TX_START（esp32s3_i2s.c L708，
   * i2s_txdma_start）→ record 只读 RX（无 TX 数据流）→ TX_START=0 →
   * TX 串行器不工作 → BCLK/WS 不输出 → ES7210 无时钟 → RX 3s 超时
   *（r=-110，实测 TX_CONF=0x08089200 bit2=0）。
   * 置位后 + STOP_EN=0 → FIFO 空也持续输出 BCLK/WS，RX 时钟稳定。 */

  modifyreg32(I2S_TX_CONF_REG(0), 0, I2S_TX_START);

  /* 排空残留信号量（官方驱动启动时可能已有完成事件） */

  while (nxsem_trywait(&g_tx_done_sem) == 0);

  return 0;
}

int hal_i2s_start_tx_clock(void)
{
  /* 官方驱动初始化时 TX 通道已启动（时钟持续输出，STOP_EN=0） */

  return 0;
}

/****************************************************************************
 * TX 播放（2026-09-02 窗口化流水线 —— "吱吱吱嘟嘟"根因修复）
 *
 * 背景：旧版逐 2044B 块"排队→等 EOF→HPWORK worker 回调→再排队"，
 * 每块之间 DMA 必然空转（worker 往返 > FIFO 余量），1kHz 纯音被切成
 * ~21ms 段、段间掉拍 → 听感"吱吱吱嘟嘟"。08-25 验证干净的 tone 是
 * 一次性大 DMA 链（无块间隙）；09-02 分块 + apb 压 2044B 后音质从未
 * 验证过——本次修复。
 *
 * 机制：排队 HAL_TX_MAX_INFLIGHT 块（官方驱动 container 池=4，留 1
 * 给 RX 边界）再等完成 → 块 EOF 后官方驱动 ISR 直接从 pend 队列续链
 * 下一块（esp32s3_i2s.c i2s_tx_schedule），无 worker 往返空窗 → DMA
 * 连续。窗口满才阻塞等一块腾位（官方驱动池满时 i2s_send 自身也阻塞，
 * 天然流控，不会死锁）。
 *
 * 注意：i2s_send 排队时驱动已把数据 memcpy 进内部缓冲（txdma_setup），
 * 故 hal_i2s_write_async 返回后调用方可安全复用 buf——tone 播放据此
 * 实现"生成下一块时上一块仍在播"，彻底消除块边界空窗。
 ****************************************************************************/

/* 排队一个 TX 块（≤2044B、帧对齐）进官方驱动 pend 队列，不等完成。
 * 返回 OK / 负 errno。container 池满时官方驱动 i2s_send 内部阻塞。 */

static int hal_tx_queue_block(FAR const void *buf, uint32_t bytes)
{
  struct audio_buf_desc_s desc;
  FAR struct ap_buffer_s *apb = NULL;
  int ret;

  memset(&desc, 0, sizeof(desc));
  desc.numbytes = bytes;
  desc.u.pbuffer = &apb;
  ret = apb_alloc(&desc);
  if (ret < 0)
    {
      return ret;
    }

  memcpy(apb->samp, buf, bytes);
  apb->nbytes = bytes;
  apb->curbyte = 0;

  ret = g_dev->ops->i2s_send(g_dev, apb, hal_i2s_tx_cb, NULL, SEC2TICK(5));
  if (ret < 0)
    {
      /* 未入队：释放我们的引用（失败路径驱动可能已 reference） */

      printf("[I2S] i2s_send 入队失败: %d (%u B)\n", ret, (unsigned)bytes);
      apb_free(apb);
      return ret;
    }

  s_tx_inflight++;
  return OK;
}

/* 等 TX 在途块数降到 want（want=0 全部排空）。超时=驱动/时钟异常：
 * 清空信号量防错乱后返回 -ETIMEDOUT。 */

static int hal_tx_wait(int want)
{
  while (s_tx_inflight > want)
    {
      if (nxsem_tickwait(&g_tx_done_sem, SEC2TICK(5)) < 0)
        {
          printf("[I2S] TX 排空超时 (inflight=%d)!\n", s_tx_inflight);
          while (nxsem_trywait(&g_tx_done_sem) == 0);
          s_tx_inflight = 0;
          return -ETIMEDOUT;
        }

      s_tx_inflight--;
    }

  return OK;
}

int hal_i2s_write_async(const void *buf, uint32_t bytes)
{
  uint32_t sent = 0;
  FAR const uint8_t *p = buf;
  int err;

  /* ⚠️ 2026-09-02：TX 播放前【同步】停预读链（-5 修复，幂等）——
   * 环形 FIFO 预读自动续会让 RX DMA 持续活动，官方驱动 i2s_send 与
   * RX 并发时 TX 启动失败。分时：播放停 RX，录音时 read_slot 自动重启。 */

  hal_i2s_rx_prefetch_stop();

  while (sent < bytes)
    {
      uint32_t chunk = MIN(bytes - sent, 2044);   /* 2044B=堆容纳上限（见预读注释） */

      chunk -= (chunk % HAL_FRAME_BYTES);   /* 对齐到帧（4B） */

      if (chunk == 0)
        {
          break;
        }

      /* 窗口满 → 等一块完成腾位（ISR 已把 pend 下一块无缝接上，无空窗） */

      if (s_tx_inflight >= HAL_TX_MAX_INFLIGHT)
        {
          err = hal_tx_wait(HAL_TX_MAX_INFLIGHT - 1);
          if (err < 0)
            {
              return err;
            }
        }

      err = hal_tx_queue_block(p + sent, chunk);
      if (err == -ENOMEM)
        {
          /* 堆紧：先排空在途（回收 apb/container）再重试一次 */

          hal_tx_wait(0);
          err = hal_tx_queue_block(p + sent, chunk);
        }

      if (err < 0)
        {
          return err;
        }

      sent += chunk;
    }

  return (int)sent;
}

int hal_i2s_write_flush(void)
{
  return hal_tx_wait(0);
}

int hal_i2s_write(const void *buf, uint32_t bytes)
{
  int ret = hal_i2s_write_async(buf, bytes);

  if (ret < 0)
    {
      return ret;
    }

  return hal_i2s_write_flush() < 0 ? -ETIMEDOUT : ret;
}

int hal_i2s_read(void *buf, uint32_t bytes)
{
  return hal_i2s_read_slot(buf, bytes, 0);
}

int hal_i2s_read_slot(void *buf, uint32_t bytes, int slot)
{
  FAR int16_t *p = (FAR int16_t *)buf;
  uint32_t want_samp = bytes / 2;
  uint32_t got = 0;
  irqstate_t flags;

  /* 确保预读链在跑且满（深度 2：首次调用 / 播放后重启自动补满） */

  hal_i2s_rx_prefetch_start(slot);

  /* 从环形 FIFO 取数：先取已有，不够等预读回调（自动续链持续供数） */

  while (got < want_samp)
    {
      flags = enter_critical_section();
      while (s_rx_fifo_cnt > 0 && got < want_samp)
        {
          p[got++] = s_rx_fifo[s_rx_fifo_rd];
          s_rx_fifo_rd = (s_rx_fifo_rd + 1) & HAL_RX_FIFO_MASK;
          s_rx_fifo_cnt--;
        }

      leave_critical_section(flags);

      if (got >= want_samp)
        {
          break;
        }

      if (nxsem_tickwait(&s_rx_prefetch_sem, SEC2TICK(3)) < 0)
        {
          printf("[I2S] RX FIFO 超时（预读链断？TX_CONF=0x%08x "
                 "RX_CONF=0x%08x）\n",
                 (unsigned)getreg32(I2S_TX_CONF_REG(0)),
                 (unsigned)getreg32(I2S_RX_CONF_REG(0)));
          break;
        }
    }

  return (int)(got * 2);
}

uint32_t hal_i2s_rx_drop_count(void)
{
  return s_rx_fifo_drop;
}

int hal_i2s_rx_set_channel(int slot)
{
  /* 官方驱动 RX 恒为 2 槽立体声；槽选择在 read_slot 完成 */

  (void)slot;
  return 0;
}

/****************************************************************************
 * 诊断：I2S TX 状态 dump（无声排查用）
 *
 * 播放"完成"却无声时判读：
 *  - TX_CONF bit2=TX_START(1=启动)、bit13=TX_STOP_EN(0=时钟保持)、
 *    bit27=SIG_LOOPBACK；
 *  - STATE bit0=TX_IDLE(1=串行器空闲——播放刚结束正常，播放中应为 0)。
 ****************************************************************************/

void hal_i2s_dump_tx(void)
{
  printf("[I2S] TX_CONF=0x%08x TX_CONF1=0x%08x STATE=0x%08x "
         "TX_CLKM=0x%08x\n",
         (unsigned)getreg32(I2S_TX_CONF_REG(0)),
         (unsigned)getreg32(I2S_TX_CONF1_REG(0)),
         (unsigned)getreg32(I2S_STATE_REG(0)),
         (unsigned)getreg32(I2S_TX_CLKM_CONF_REG(0)));
  printf("[I2S] TX_START=%u TX_STOP_EN=%u TX_IDLE=%u SIG_LOOPBACK=%u\n",
         (unsigned)((getreg32(I2S_TX_CONF_REG(0)) & I2S_TX_START) ? 1 : 0),
         (unsigned)((getreg32(I2S_TX_CONF_REG(0)) & I2S_TX_STOP_EN) ? 1 : 0),
         (unsigned)((getreg32(I2S_STATE_REG(0)) & I2S_TX_IDLE) ? 1 : 0),
         (unsigned)((getreg32(I2S_TX_CONF_REG(0)) & I2S_SIG_LOOPBACK) ? 1 : 0));

  /* ⚠️ 2026-09-02 加 RX 帧结构（ES7210 4 槽/64×FS 判据）：
   * RX_TDM_CTRL bits[19:16]=TOT_CHAN_NUM(3=4槽) 低16位=chan 使能；
   * RX_CLKM div 应使 BCLK=1.536MHz（24k×4×16）。 */

  printf("[I2S] RX_TDM_CTRL=0x%08x RX_CONF=0x%08x RX_CLKM=0x%08x\n",
         (unsigned)getreg32(I2S_RX_TDM_CTRL_REG(0)),
         (unsigned)getreg32(I2S_RX_CONF_REG(0)),
         (unsigned)getreg32(I2S_RX_CLKM_CONF_REG(0)));

  /* ⚠️ 2026-09-02 加 I2S 中断原态（播放欠载判据）：
   * INT_RAW bit3=TX_HUNG（TX FIFO 空转超阈值 = 播放期间 DMA 供数
   * 断过）、bit1=TX_DONE、bit0=RX_DONE。播放刚结束 TX_HUNG=1 说明
   * 数据流有洞（块间空窗），连续播放应全 0。 */

  printf("[I2S] INT_RAW=0x%08x (TX_HUNG=%u TX_DONE=%u RX_DONE=%u)\n",
         (unsigned)getreg32(I2S_INT_RAW_REG(0)),
         (unsigned)((getreg32(I2S_INT_RAW_REG(0)) & I2S_TX_HUNG_INT_RAW) ? 1 : 0),
         (unsigned)((getreg32(I2S_INT_RAW_REG(0)) & I2S_TX_DONE_INT_RAW) ? 1 : 0),
         (unsigned)((getreg32(I2S_INT_RAW_REG(0)) & I2S_RX_DONE_INT_RAW) ? 1 : 0));
}
