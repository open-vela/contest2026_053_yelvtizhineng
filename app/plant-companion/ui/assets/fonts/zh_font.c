/****************************************************************************
 * apps/plant-companion/ui/assets/fonts/zh_font.c
 *
 * 中文字体统一加载层（CLAUDE_SYSTEM.md §14）
 *
 * ⚠️ 2026-09-02 修复「SD 全量字库整体切换」回归（实机反馈：时间/数字全方块
 * + 切页卡顿）：
 *   - 旧实现：SD 字库可用时把 TP_FONT_* 整体换成 SD 字体——SD 字库只含
 *     GB2312 汉字+全角标点（无 ASCII 数字/冒号/百分号/空格）→ 时间"10:25"、
 *     电池"87%"等全变 LVGL 缺字占位方框；且每字 13 次 fseek+fread 在
 *     ui_task 上同步查 SD，整页文字 = 几百次随机读 → 跳转慢。
 *   - 新实现（两级字体，LVGL 原生 fallback 语义）：
 *       主字体 = flash 子集（lv_font_plant_zh_* 运行时副本）：含 ASCII +
 *        250 个静态 UI 汉字，XIP 直读零拷贝、不碰 SD → 时间/数字/静态页
 *        全部恢复且快；
 *       fallback = SD 全量字库（/mnt/sd/fonts/plant_zh_<size>.bin）：
 *        子集外汉字（AI 回复/诊断/日记自由文本）才经 SD 流式二分查字形。
 *     两者 line_height/base_line 一致（16→20/5, 20→24/6, 24→29/7），
 *     布局零变化；SD 文件缺失/拔卡 → SD 兜底断开，行为 = flash 子集 +
 *     符号/emoji 兜底。
 *   - 2026-09-03 三级扩展（修图标/emoji 方块）：主字体/SD 之后追加
 *       montserrat(符号 U+F0xx) → lv_font_emoji(真 emoji)，见
 *       zh_bind_icon_fonts；SD 不可用时主字体直挂该链，方块不再出现。
 *     安全资源配置（硬约束，违反会翻车）：
 *   - 字形缓存：lv_malloc（LVGL 池内）一次性 ≤10KB，32 槽 FIFO/LRU——
 *     ⚠️ 绝不用静态 .bss（会推 _sheap 挤爆堆，见 CLAUDE_SYSTEM §6 静态栈翻车）
 *   - 索引不常驻 RAM：流式二分 fseek+fread（16B × ~13 次/新字；命中缓存
 *     后 0 次——同一字形在测量/绘制/逐帧重绘间反复查询，memo 消掉重复二分）
 *   - 查找/读取全静态小缓冲（无 malloc 大块、无大栈局部）
 *   - 字库文件缺失/拔卡/缓存分配失败 → 仅 flash 子集（等价旧阶段 A），不崩
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "zh_font.h"
#include "../../theme/theme_plant.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static zh_font_src_t g_src = ZH_SRC_FLASH_SUBSET;

/* ⚠ 2026-09-11：字体归属任务（= ui_task）。
 *
 * 背景：SD 兜底字库的 FILE* 按 NuttX 任务组隔离（见下方 fd 隔离说明）。
 * UI 起来后（SD 已挂载）在 NSH 里跑 `plant font status` 会绕过上面那句
 * "g_src==SD_FULL 直接返回"的保护：此时 g_src 还是 FLASH_SUBSET（UI 启动
 * 时 SD 未挂载，探测失败）→ 于是 NSH 里重新 fopen 三个 SD 字库，并把
 * s_main16/19/25 的 fallback 指向本任务组（NSH）的 FILE*。
 * 后果：ui_task 随后绘制任何 SD 兜底汉字都会 fseek 到别人家的句柄
 * → 句柄/堆被搞烂 → 约 8s 后 HardFault panic（EXCCAUSE=0014，PC 飞掉）。
 * 实机证据：_work/font_status_1.log。
 *
 * 修法：只有归属任务（首次调用 zh_font_bind_ui 的那个任务 = ui_task）才
 * 允许重开句柄/重挂 fallback 链；其它任务的 zh_font_init() 一律直接返回，
 * 诊断命令退化为"只读"（缺字判断请用 plant objs，跑在 UI 上下文）。 */
static volatile pid_t g_ui_pid;
static int g_retry_left = 12;   /* UI 上下文重试次数（SD 挂载晚于 UI 启动） */

/* ── SD 全量字库（主字体的 fallback）──────────────────────────── */

#define ZH_SD_PATH_FMT   "/mnt/sd/fonts/plant_zh_%d.bin"
#define ZH_SD_HEADER     24     /* MAGIC(8)+ver(2)+size(2)+count(4)+idx(4)+lh(2)+bl(2) */
#define ZH_SD_IDX_ENTRY  16     /* unicode u32|offset u32|size u16|adv u16|w u8|h u8|ox i8|oy i8 */
#define ZH_SD_CACHE_ENTS 12     /* 缓存槽（FIFO/LRU 覆盖），12×320B=3.8KB（64KB 池内存回归修复，2026-09-07） */
#define ZH_SD_CACHE_SLOT 320    /* 每槽字节上限（24px 字形 ≤288B + 余量） */
#define ZH_SD_CACHE_MAX  (ZH_SD_CACHE_ENTS * ZH_SD_CACHE_SLOT)  /* 3840B ≤10KB */

typedef struct
{
  FILE *fp;               /* 所属字库文件（key 之一） */
  uint32_t glyph_index;   /* 索引表序号（key 之二） */
  uint16_t last_use;      /* LRU 时间戳 */
} zh_sd_slot_t;

typedef struct
{
  uint8_t *cache;         /* lv_malloc(ZH_SD_CACHE_MAX)，三字号共享 */
  uint16_t tick;          /* LRU 时钟 */
  zh_sd_slot_t slots[ZH_SD_CACHE_ENTS];
} zh_sd_cache_t;

static zh_sd_cache_t s_zh_cache;

typedef struct
{
  FILE *fp;               /* 字库文件句柄（open 一次） */
  uint32_t count;         /* 字形总数 */
  uint32_t idx_off;       /* 数据区起点 */
  int size_px;
  /* 末次命中 memo（unicode==0 无效）：同一字形在文本测量/绘制/逐帧重绘间
   * 被反复查询，命中即免掉一次 13 次 fseek 的二分——SD 慢的关键消减项 */
  uint32_t m_unicode;
  uint32_t m_gidx;
  uint32_t m_offset;
  uint16_t m_size;
  uint16_t m_adv;
  uint8_t m_box_w;
  uint8_t m_box_h;
  int8_t m_ofs_x;
  int8_t m_ofs_y;
} zh_sd_font_t;

static zh_sd_font_t s_sd_font16;
static zh_sd_font_t s_sd_font20;
static zh_sd_font_t s_sd_font24;

/* ⚠ 2026-09-10 实测结论（重要，勿再踩）：NuttX 的 fd 表按任务组隔离——
 * 句柄在哪个任务里 fopen，就只能在那个任务里用。zh_font_init 由 UI 线程
 * 调用，所以 UI 绘制路径一直正常；而在 NSH 里跑的 plant font status/trace/sd
 * 拿同一个 FILE* 去读必然得到 EBADF —— 用它判"缺字"会误报成全缺。
 * 同理：绝不能对另一个任务组打开的 FILE* 调 fclose —— 它只会关掉当前
 * 组 fd 表里同号码的另一个文件（实测：直接把堆搞崩，系统挂）。
 * 因此：UI 路径持有句柄；诊断命令只读不重开、不关闭。 */

static bool zh_sd_get_glyph_dsc(const lv_font_t *font,
                                lv_font_glyph_dsc_t *dsc,
                                uint32_t letter, uint32_t letter_next);
static const void *zh_sd_get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc,
                                          lv_draw_buf_t *dbuf);

/* 三套 SD 字体的 lv_font_t（非 const：需挂 montserrat/emoji fallback，
 * 见 zh_bind_icon_fonts）。line_height/base_line 固定 =
 * 现有 flash 子集字体的值（16→20/5, 20→24/6, 24→29/7）→ 切换后 UI 行高/
 * 基线零变化（生成脚本的 ofs_y 已按 LVGL 语义 ascent-字形底 生成）。 */

static lv_font_t s_sd_lvfont16 =
{
  .get_glyph_dsc = zh_sd_get_glyph_dsc,
  .get_glyph_bitmap = zh_sd_get_glyph_bitmap,
  .line_height = 20,
  .base_line = 5,
  .user_data = &s_sd_font16,
};

static lv_font_t s_sd_lvfont20 =
{
  .get_glyph_dsc = zh_sd_get_glyph_dsc,
  .get_glyph_bitmap = zh_sd_get_glyph_bitmap,
  .line_height = 24,
  .base_line = 6,
  .user_data = &s_sd_font20,
};

static lv_font_t s_sd_lvfont24 =
{
  .get_glyph_dsc = zh_sd_get_glyph_dsc,
  .get_glyph_bitmap = zh_sd_get_glyph_bitmap,
  .line_height = 29,
  .base_line = 7,
  .user_data = &s_sd_font24,
};

/* ── 主字体（flash 子集运行时对象）──────────────────────────────
 * lv_font_plant_zh_* 是 const（XIP 直读），但两级字体需要给主字体挂
 * .fallback → 结构拷贝一份到 RAM（~几十字节/套，仅字段指针，非字形数据）。
 * zh_font_get 永远返回这三个对象：SD 兜底可用时 .fallback = s_sd_lvfont*，
 * LVGL 查缺字形自动走 fallback 链（wrapper 把 resolved_font 指到实际命中
 * 的字体 → 位图由对应实现取，无需自研分发）。2026-09-03 起 SD 链尾再挂
 * montserrat(符号)→emoji（zh_bind_icon_fonts），SD 不可用则主字体直挂该链，
 * 保证图标/emoji 永不落占位方块（见函数头注释）。 */

static lv_font_t s_main12;
static lv_font_t s_main16;
static lv_font_t s_main19;
static lv_font_t s_main25;

/* ── 符号 + emoji 兜底（2026-09-03：修复全 UI 图标/emoji 渲染成方块）──
 * 主字体（zh 子集 lv_font_plant_zh_*）码位只到 U+FF1F（ASCII+250 汉字），
 * SD 全量字库只含汉字 → LV_SYMBOL（U+F0xx，字形在 LVGL montserrat 里）
 * 与真 emoji（U+2600+/U+1F3xx+，字形在 lv_font_emoji_*，OpenMoji 单色
 * 子集，见 ui/assets/fonts/ 生成命令）全部落空 → LVGL 画占位方块。
 * LVGL fallback 是单链指针 → 逐级串（每级只挂下一级）：
 *   zh 主字体 → [SD 全量汉字(可选)] → montserrat(符号) → emoji
 * montserrat/emoji 为 const（flash 直读），要挂下一级 fallback 必须
 * RAM 结构副本（仅字段指针，非字形数据，开销几十字节）。 */

extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_20;
extern const lv_font_t lv_font_emoji_16;
extern const lv_font_t lv_font_emoji_20;
extern const lv_font_t lv_font_emoji_24;
extern const lv_font_t lv_font_emoji_32;

static lv_font_t s_icon_s;   /* montserrat_14 副本 → emoji16（12/16px 链尾） */
static lv_font_t s_icon_m;   /* montserrat_20 副本 → emoji20（19px 链尾） */
static lv_font_t s_icon_l;   /* montserrat_20 副本 → emoji24（25px 链尾） */
static lv_font_t s_icon_xl;  /* montserrat_20 副本 → emoji32（大图标链尾） */

/* 幂等：结构拷贝 flash 子集字体（首次调用执行） */

static void zh_bind_main_fonts(void)
{
  if (s_main16.get_glyph_dsc == NULL)
    {
      s_main12 = lv_font_plant_zh_12;
      s_main16 = lv_font_plant_zh_16;
      s_main19 = lv_font_plant_zh_19;
      s_main25 = lv_font_plant_zh_25;
    }
}

/* 幂等：结构拷贝 montserrat 副本并挂 emoji 链尾（首次调用执行） */

static void zh_bind_icon_fonts(void)
{
  if (s_icon_s.get_glyph_dsc == NULL)
    {
      s_icon_s = lv_font_montserrat_14;
      s_icon_s.fallback = &lv_font_emoji_16;
      s_icon_m = lv_font_montserrat_20;
      s_icon_m.fallback = &lv_font_emoji_20;
      s_icon_l = lv_font_montserrat_20;
      s_icon_l.fallback = &lv_font_emoji_24;
      s_icon_xl = lv_font_montserrat_20;
      s_icon_xl.fallback = &lv_font_emoji_32;
    }
}

/****************************************************************************
 * Private Functions（阶段 B：SD 字库）
 ****************************************************************************/

/* 流式二分：unicode → 索引项。返回 false = 字不在字库。 */

static bool zh_sd_lookup(zh_sd_font_t *f, uint32_t unicode,
                         uint32_t *glyph_index, uint32_t *offset,
                         uint16_t *size, uint16_t *adv,
                         uint8_t *box_w, uint8_t *box_h,
                         int8_t *ofs_x, int8_t *ofs_y)
{
  uint32_t lo = 0;
  uint32_t hi = f->count;
  uint8_t b[ZH_SD_IDX_ENTRY];

  /* memo 快路径：同一字形反复查（测量→绘制→逐帧重绘）免掉整个二分 */

  if (f->m_unicode == unicode)
    {
      *glyph_index = f->m_gidx;
      *offset = f->m_offset;
      *size = f->m_size;
      *adv = f->m_adv;
      *box_w = f->m_box_w;
      *box_h = f->m_box_h;
      *ofs_x = f->m_ofs_x;
      *ofs_y = f->m_ofs_y;
      return true;
    }

  while (lo < hi)
    {
      uint32_t mid = (lo + hi) / 2;
      uint32_t u;

      if (fseek(f->fp, ZH_SD_HEADER + (long)mid * ZH_SD_IDX_ENTRY,
                SEEK_SET) != 0)
        {
          return false;
        }

      if (fread(b, 1, ZH_SD_IDX_ENTRY, f->fp) != ZH_SD_IDX_ENTRY)
        {
          return false;
        }

      u = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
          ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);

      if (u < unicode)
        {
          lo = mid + 1;
        }
      else if (u > unicode)
        {
          hi = mid;
        }
      else
        {
          *glyph_index = mid;
          *offset = (uint32_t)b[4] | ((uint32_t)b[5] << 8) |
                    ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
          *size = (uint16_t)(b[8] | (b[9] << 8));
          *adv = (uint16_t)(b[10] | (b[11] << 8));
          *box_w = b[12];
          *box_h = b[13];
          *ofs_x = (int8_t)b[14];
          *ofs_y = (int8_t)b[15];

          f->m_unicode = unicode;
          f->m_gidx = mid;
          f->m_offset = *offset;
          f->m_size = *size;
          f->m_adv = *adv;
          f->m_box_w = *box_w;
          f->m_box_h = *box_h;
          f->m_ofs_x = *ofs_x;
          f->m_ofs_y = *ofs_y;
          return true;
        }
    }

  return false;
}

/* 字形位图：查缓存（LRU）→ 未命中 fseek+fread 读入最旧槽 */

static const uint8_t *zh_sd_glyph_data(zh_sd_font_t *f, uint32_t glyph_index,
                                       uint32_t offset, uint16_t size)
{
  int i;
  int oldest = 0;
  uint16_t min_tick = 0xffff;

  if (size > ZH_SD_CACHE_SLOT)
    {
      return NULL;
    }

  /* 懒分配（2026-09-07 内存回归修复）：LVGL 池仅 64KB，10KB 常驻缓存把
   * home 建好后 free 压到 ~4KB → 点其它页内存耗尽卡死。改为首次真正要
   * 取 SD 字形（含子集外汉字的页面绘制）时才从池里分配，并缩到 12 槽。 */
  if (s_zh_cache.cache == NULL)
    {
      s_zh_cache.cache = lv_malloc(ZH_SD_CACHE_MAX);
      if (s_zh_cache.cache == NULL)
        {
          return NULL;
        }

      memset(&s_zh_cache.slots, 0, sizeof(s_zh_cache.slots));
    }

  for (i = 0; i < ZH_SD_CACHE_ENTS; i++)
    {
      if (s_zh_cache.slots[i].fp == f->fp &&
          s_zh_cache.slots[i].glyph_index == glyph_index)
        {
          s_zh_cache.slots[i].last_use = ++s_zh_cache.tick;
          return s_zh_cache.cache + (size_t)i * ZH_SD_CACHE_SLOT;
        }

      if (s_zh_cache.slots[i].last_use < min_tick)
        {
          min_tick = s_zh_cache.slots[i].last_use;
          oldest = i;
        }
    }

  if (fseek(f->fp, (long)f->idx_off + offset, SEEK_SET) != 0)
    {
      return NULL;
    }

  if (fread(s_zh_cache.cache + (size_t)oldest * ZH_SD_CACHE_SLOT,
            1, size, f->fp) != size)
    {
      return NULL;
    }

  s_zh_cache.slots[oldest].fp = f->fp;
  s_zh_cache.slots[oldest].glyph_index = glyph_index;
  s_zh_cache.slots[oldest].last_use = ++s_zh_cache.tick;
  return s_zh_cache.cache + (size_t)oldest * ZH_SD_CACHE_SLOT;
}

static bool zh_sd_get_glyph_dsc(const lv_font_t *font,
                                lv_font_glyph_dsc_t *dsc,
                                uint32_t letter, uint32_t letter_next)
{
  zh_sd_font_t *f = (zh_sd_font_t *)font->user_data;
  uint32_t glyph_index;
  uint32_t offset;
  uint16_t size;
  uint16_t adv;
  uint8_t box_w;
  uint8_t box_h;
  int8_t ofs_x;
  int8_t ofs_y;
  (void)letter_next;

  if (f == NULL || f->fp == NULL)
    {
      return false;
    }

  if (!zh_sd_lookup(f, letter, &glyph_index, &offset, &size, &adv,
                    &box_w, &box_h, &ofs_x, &ofs_y))
    {
      return false;
    }

  /* gid.index 传索引表序号（非 unicode）：get_glyph_bitmap 凭它一次
   * fseek 读索引项拿 offset/size，避免二次二分 */

  /* 字库 adv_w 单位 = 1/10 px（tools/generate_font_bin.py:
   * adv = round(font.getlength(ch) * 10)），LVGL 运行时要求 px。
   * ⚠️ 2026-09-10 修复：此前直接传 1/10px 原值（20px 汉字 200 →
   * 200px 步进）→ 字与字之间出现巨大空白。 */

  dsc->adv_w = (uint16_t)((adv + 5) / 10);
  dsc->box_w = box_w;
  dsc->box_h = box_h;
  dsc->ofs_x = ofs_x;
  dsc->ofs_y = ofs_y;
  dsc->format = LV_FONT_GLYPH_FORMAT_A4;
  dsc->is_placeholder = 0;
  dsc->gid.index = glyph_index;
  dsc->resolved_font = font;
  return true;
}

static const void *zh_sd_get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc,
                                          lv_draw_buf_t *dbuf)
{
  zh_sd_font_t *f;
  uint8_t b[ZH_SD_IDX_ENTRY];
  uint32_t offset;
  uint16_t size;
  const uint8_t *raw;
  uint32_t y;
  uint32_t x;
  uint32_t stride;
  uint32_t row_bytes;
  uint8_t *out;

  /* ⚠️ 2026-09-02 修复（开机首帧渲染崩溃根因，误诊"池必须 80KB"）：
   * LVGL v9 字体契约 = 把字形【解码进调用方 dbuf->data】并返回 dbuf
   * （对齐内置 lv_font_get_bitmap_fmt_txt：目标 A8、stride 行距）。
   * 旧实现忽略 dbuf、直接返回裸缓存指针 → lv_draw_label 的 letter 层
   * 把该裸指针当 lv_draw_buf_t 解析（blend mask_buf = 指针->data =
   * 字形像素前几字节）→ mask_buf=垃圾(0xa8xxxxxx) → blend_color
   * LoadProhibited 崩溃。SD 挂载后首帧画任何中文即崩；SD 未挂时用
   * flash 子集字体（契约正确）不崩——与池大小无关（64KB 时代崩溃
   * 同样是 SD 字库触发，池 80KB 结论是巧合）。 */

  if (g_dsc->resolved_font == NULL || g_dsc->resolved_font->user_data == NULL ||
      dbuf == NULL)
    {
      return NULL;
    }

  f = (zh_sd_font_t *)g_dsc->resolved_font->user_data;
  if (f->fp == NULL)
    {
      return NULL;
    }

  /* 由 glyph_index（表序号）读索引项拿 offset/size */

  if (fseek(f->fp, ZH_SD_HEADER + (long)g_dsc->gid.index * ZH_SD_IDX_ENTRY,
            SEEK_SET) != 0)
    {
      return NULL;
    }

  if (fread(b, 1, ZH_SD_IDX_ENTRY, f->fp) != ZH_SD_IDX_ENTRY)
    {
      return NULL;
    }

  offset = (uint32_t)b[4] | ((uint32_t)b[5] << 8) |
           ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
  size = (uint16_t)(b[8] | (b[9] << 8));

  raw = zh_sd_glyph_data(f, g_dsc->gid.index, offset, size);
  if (raw == NULL)
    {
      return NULL;
    }

  /* SD 字库 A4(4bit/px) 打包：每行 (box_w+1)/2 字节、高 nibble 在前
   *（generate_font_bin.py 同款）→ 逐行解成 A8（nibble<<4|nibble）
   * 写入 dbuf（LVGL 按 box_w×box_h A8 建缓冲）。 */

  out = dbuf->data;
  stride = lv_draw_buf_width_to_stride(g_dsc->box_w, LV_COLOR_FORMAT_A8);
  row_bytes = ((uint32_t)g_dsc->box_w + 1) / 2;

  for (y = 0; y < (uint32_t)g_dsc->box_h; y++)
    {
      const uint8_t *src = raw + y * row_bytes;

      for (x = 0; x < (uint32_t)g_dsc->box_w; x++)
        {
          uint8_t nib = (x & 1) ? (src[x >> 1] & 0x0f)
                                : (src[x >> 1] >> 4);

          out[x] = (uint8_t)((nib << 4) | nib);
        }

      out += stride;
    }

  return dbuf;
}

/* 打开 SD 字库（一个字号）；失败返回负 errno */

static int zh_sd_open_one(zh_sd_font_t *f, int size_px)
{
  char path[64];
  uint8_t hdr[ZH_SD_HEADER];

  /* 只置空指针、不在这里 fclose：上次的句柄可能属于另一个任务组
   * （见上方 fd 隔离说明），关它会关错当前组的 fd。 */

  f->fp = NULL;
  f->m_unicode = 0;

  snprintf(path, sizeof(path), ZH_SD_PATH_FMT, size_px);

  /* NuttX FAT：<=8.3 目录落盘为 8.3 大写短名（FONTS），且本版本对短名
   * 大小写敏感 → Windows 建的 "fonts" 小写目录查不到。先按工程原布局
   * 试小写，失败静默改试大写，兼容两种备卡方式。 */
  f->fp = fopen(path, "rb");
  if (f->fp == NULL)
    {
      char uppath[64];

      snprintf(uppath, sizeof(uppath), "/mnt/sd/FONTS/plant_zh_%d.bin",
               size_px);
      f->fp = fopen(uppath, "rb");
      if (f->fp != NULL)
        {
          snprintf(path, sizeof(path), "%s", uppath);
        }
    }

  if (f->fp == NULL)
    {
      printf("[Font] 打开 %s 失败 errno=%d（SD 未挂/文件缺失/读取 EIO）\n",
             path, errno);
      return -errno;
    }

  if (fread(hdr, 1, ZH_SD_HEADER, f->fp) != ZH_SD_HEADER ||
      memcmp(hdr, "PLANTZH1", 8) != 0)
    {
      printf("[Font] %s 头校验失败（文件损坏/格式不符）\n", path);
      fclose(f->fp);
      f->fp = NULL;
      return -EINVAL;
    }

  f->count = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
             ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
  f->idx_off = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) |
               ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);
  f->size_px = size_px;
  f->m_unicode = 0;   /* 清 memo（文件刚打开/换文件） */
  return 0;
}

static void zh_sd_close(void)
{
  if (s_sd_font16.fp != NULL)
    {
      fclose(s_sd_font16.fp);
      s_sd_font16.fp = NULL;
    }

  if (s_sd_font20.fp != NULL)
    {
      fclose(s_sd_font20.fp);
      s_sd_font20.fp = NULL;
    }

  if (s_sd_font24.fp != NULL)
    {
      fclose(s_sd_font24.fp);
      s_sd_font24.fp = NULL;
    }

  if (s_zh_cache.cache != NULL)
    {
      lv_free(s_zh_cache.cache);
      s_zh_cache.cache = NULL;
    }

  memset(&s_zh_cache.slots, 0, sizeof(s_zh_cache.slots));
}

/****************************************************************************
 * Private Functions（UTF-8）
 ****************************************************************************/

static uint32_t zh_utf8_decode(const char *s, int *len)
{
  const uint8_t *p = (const uint8_t *)s;

  if (p[0] < 0x80)
    {
      *len = 1;
      return p[0];
    }

  if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80)
    {
      *len = 2;
      return ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
    }

  if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80)
    {
      *len = 3;
      return ((uint32_t)(p[0] & 0x0f) << 12) |
             ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
    }

  if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 &&
      (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80)
    {
      *len = 4;
      return ((uint32_t)(p[0] & 0x07) << 18) |
             ((uint32_t)(p[1] & 0x3f) << 12) |
             ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
    }

  *len = 1;
  return 0;
}


/* 调试探测（2026-09-07）：zh_font_init 完成后直接走 LVGL 字形查找链
 * （与 UI 绘制同一路径），验证 flash 子集之外的字/emoji 运行时能否解析。 */
static void zh_font_diag_probe(void)
{
  static const struct
  {
    const char *name;
    uint32_t cp;
  } probe[] =
  {
    { "yi",     0x4E00 },   /* yi */
    { "you",    0x53C8 },   /* you */
    { "yuan",   0x56ED },   /* yuan */
    { "ding",   0x4E01 },   /* ding */
    { "ai",     0x7231 },   /* ai */
    { "man",    0x6EE1 },   /* man */
    { "zhuan",  0x4E13 },   /* zhuan */
    { "jia",    0x5BB6 },   /* jia */
    { "middot", 0x00B7 },   /* middot */
    { "lbrack", 0x300C },   /* lbrack */
    { "medal",  0x1F3C5 },  /* medal */
    { "ring",   0x2B55 },   /* ring */
    { "tada",   0x1F389 },  /* tada */
  };
  int size_tab[2] = { 16, 20 };
  int si;
  int i;

  /* 2026-09-08: 开机自检改为一行摘要。完整逐字形/位图转储会向无人读取的
   * USB-Serial-JTAG 控制台灌 ~3KB，写满 4096B FIFO 后 printf 永久阻塞
   * ui_task -> 界面"点没反应"。需要深查字体时临时打开下方 return。 */
  printf("[Font] boot glyph probe ok (src=%d, flash+SD+emoji fallback)\n",
         (int)g_src);
  return;

  printf("[Font][Dbg] src=%d main16.fb=%p main20.fb=%p sd16fp=%p sd20fp=%p"
         " sd16.fb=%p sd20.fb=%p icon16.fb=%p\n",
         (int)g_src, (void *)s_main16.fallback, (void *)s_main19.fallback,
         (void *)s_sd_font16.fp, (void *)s_sd_font20.fp,
         (void *)s_sd_lvfont16.fallback, (void *)s_sd_lvfont20.fallback,
         (void *)s_icon_s.fallback);

  for (si = 0; si < 2; si++)
    {
      const lv_font_t *font = zh_font_get(size_tab[si]);

      for (i = 0; i < (int)(sizeof(probe) / sizeof(probe[0])); i++)
        {
          lv_font_glyph_dsc_t dsc;
          bool ok = lv_font_get_glyph_dsc(font, &dsc, probe[i].cp, 0);
          const char *who = "NONE";

          if (!ok || dsc.resolved_font == NULL)
            {
              who = "MISS";
            }
          else if (dsc.resolved_font == font)
            {
              who = "main";
            }
          else if (dsc.resolved_font == &s_sd_lvfont16 ||
                   dsc.resolved_font == &s_sd_lvfont20 ||
                   dsc.resolved_font == &s_sd_lvfont24)
            {
              who = "sd";
            }
          else if (dsc.resolved_font == &s_icon_s ||
                   dsc.resolved_font == &s_icon_m ||
                   dsc.resolved_font == &s_icon_l ||
                   dsc.resolved_font == &s_icon_xl)
            {
              who = "montserrat";
            }
          else if (dsc.resolved_font == &lv_font_emoji_16 ||
                   dsc.resolved_font == &lv_font_emoji_20 ||
                   dsc.resolved_font == &lv_font_emoji_24)
            {
              who = "emoji";
            }
          else
            {
              who = "other";
            }

          printf("[Font][Dbg] %2dpx %-8s U+%04X %s -> %s\n",
                 size_tab[si], probe[i].name, (unsigned)probe[i].cp,
                 ok ? "FOUND" : "MISS", who);
        }
    }

  /* 字形位图解码自检（2026-09-07）：直接走 lv_font_get_glyph_bitmap 取
   * SD 字形并打点阵，验证 A4->A8 解码在实机是否产出正确字形。 */
  {
    static const uint32_t bmp_probe[3] = { 0x53C8, 0x7231, 0x4E00 };
    int bpi;

    for (bpi = 0; bpi < 3; bpi++)
      {
        lv_font_glyph_dsc_t g;
        const lv_font_t *font = zh_font_get(16);
        lv_draw_buf_t *db;
        const uint8_t *got;
        uint32_t stride;
        uint32_t yy;

        if (!lv_font_get_glyph_dsc(font, &g, bmp_probe[bpi], 0) ||
            g.resolved_font == NULL)
          {
            printf("[Font][Dbg] bmp U+%04X lookup MISS\n",
                   (unsigned)bmp_probe[bpi]);
            continue;
          }

        printf("[Font][Dbg] bmp U+%04X box=%ux%u adv=%u ofs=%d,%d fmt=%d\n",
               (unsigned)bmp_probe[bpi], (unsigned)g.box_w, (unsigned)g.box_h,
               (unsigned)g.adv_w, (int)g.ofs_x, (int)g.ofs_y, (int)g.format);

        db = lv_draw_buf_create_ex(lv_draw_buf_get_font_handlers(),
                                   g.box_w, g.box_h,
                                   LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
        if (db == NULL)
          {
            printf("[Font][Dbg] bmp U+%04X drawbuf NULL\n",
                   (unsigned)bmp_probe[bpi]);
            continue;
          }

        got = (const uint8_t *)lv_font_get_glyph_bitmap(&g, db);
        if (got == NULL)
          {
            printf("[Font][Dbg] bmp U+%04X bitmap NULL\n",
                   (unsigned)bmp_probe[bpi]);
            lv_draw_buf_destroy(db);
            continue;
          }

        stride = lv_draw_buf_width_to_stride(g.box_w, LV_COLOR_FORMAT_A8);
        for (yy = 0; yy < (uint32_t)g.box_h && yy < 26; yy++)
          {
            char line[40];
            uint32_t xx;
            uint32_t n = g.box_w;

            if (n > 32)
              {
                n = 32;
              }

            for (xx = 0; xx < n; xx++)
              {
                uint8_t v = got[(size_t)yy * stride + xx];

                line[xx] = (v >= 64) ? '#' : (v >= 8 ? '+' : '.');
              }
            line[n] = '\0';
            printf("[Font][Dbg]   %s\n", line);
          }

        lv_draw_buf_destroy(db);
      }
  }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void zh_font_init(void)
{
  /* 非归属任务（NSH 诊断命令）：只读返回，绝不动句柄/fallback 链 */

  if (g_ui_pid != 0 && getpid() != (pid_t)g_ui_pid)
    {
      return;
    }

  /* ⚠️ 并发安全：SD 兜底已就绪则直接返回（UI 线程可能正在用 SD 字体绘制，
   * NSH 的 plant font status 会调本函数——重开/重挂会 fclose 正在用的 FILE*
   * → ui_task 上 fseek 悬挂句柄会崩）。SD 未就绪时的"重试"仍保留：
   * 每次调用重新探测（UI 启动时 SD 可能未挂载/时序未稳，实测有先例）。 */

  if (g_src == ZH_SRC_SD_FULL)
    {
      return;
    }

  /* 主字体对象：flash 子集 const → 运行时副本（只改 fallback 字段）。
   * 幂等：结构拷贝只做一次。符号/emoji 副本（montserrat+emoji 链尾）同。 */

  zh_bind_main_fonts();
  zh_bind_icon_fonts();

  /* 先断开旧 fallback（上次探测失败可能已打开部分文件） */

  s_main12.fallback = NULL;
  s_main16.fallback = NULL;
  s_main19.fallback = NULL;
  s_main25.fallback = NULL;

  /* ⚠️ SD 兜底的字形渲染依赖 LVGL 池缓存（lv_malloc）——NSH 命令场景
   * （plant font status）不启动 LVGL，无缓存池 → SD 不可用，正确降级
   * flash 子集。UI（产品模式）已 lv_init，SD 兜底正常生效。
   * 2026-09-03：NSH 分支也挂 montserrat→emoji 链（纯 const 查表，不依赖
   * LVGL 池）——否则 plant font status 会误报 emoji/符号缺字（实际 UI 有）。 */

  if (!lv_is_initialized())
    {
      s_main12.fallback = &s_icon_s;
      s_main16.fallback = &s_icon_s;
      s_main19.fallback = &s_icon_m;
      s_main25.fallback = &s_icon_l;
      printf("[Font] LVGL 未初始化（NSH 场景），flash 子集 + 符号/emoji 兜底"
             "（无 SD 汉字兜底）\n");
      return;
    }

  /* 重新探测前先关掉上次的部分状态（防泄漏 FILE* 句柄/LVGL 池缓存） */

  zh_sd_close();

  /* 尝试 SD 全量字库（三字号全在 + 缓存分配成功才挂 fallback；
   * 任一失败断开 SD 兜底，仅 flash 子集，不崩） */

  if (zh_sd_open_one(&s_sd_font16, 16) == 0 &&
      zh_sd_open_one(&s_sd_font20, 20) == 0 &&
      zh_sd_open_one(&s_sd_font24, 24) == 0)
    {
      s_main12.fallback = &s_sd_lvfont16;
      s_main16.fallback = &s_sd_lvfont16;
      s_main19.fallback = &s_sd_lvfont20;
      s_main25.fallback = &s_sd_lvfont24;
      g_src = ZH_SRC_SD_FULL;
      printf("[Font] SD 全量字库就绪（flash 子集主字体 + SD 兜底，"
             "SD 覆盖 %u 字，字形缓存按需分配 %d B）\n",
             (unsigned)s_sd_font20.count, ZH_SD_CACHE_MAX);
    }
  else
    {
      zh_sd_close();
    }

  /* 符号/emoji 兜底（2026-09-03）：SD 汉字 miss 之后 → montserrat(符号)
   * → emoji；SD 不可用时主字体直挂符号链。即"无 SD 卡"也不再有方块：
   * zh → montserrat → emoji 仍完整。 */

  s_sd_lvfont16.fallback = &s_icon_s;
  s_sd_lvfont20.fallback = &s_icon_m;
  s_sd_lvfont24.fallback = &s_icon_l;

  if (g_src != ZH_SRC_SD_FULL)
    {
      s_main12.fallback = &s_icon_s;
      s_main16.fallback = &s_icon_s;
      s_main19.fallback = &s_icon_m;
      s_main25.fallback = &s_icon_l;
      printf("[Font] SD 全量字库不可用——主字体直挂符号/emoji 兜底"
             "（montserrat+emoji，无方块）\n");
    }
  zh_font_diag_probe();
}

/* 绑定当前任务为字体归属任务（= ui_task）并初始化。必须在 lv_init 之后由
 * UI 线程调用一次；此后其它任务的 zh_font_init() 全部变成空操作。 */

void zh_font_bind_ui(void)
{
  if (g_ui_pid == 0)
    {
      g_ui_pid = getpid();
    }

  zh_font_init();
}

/* UI 上下文重试挂 SD 全量字库（SD 卡挂载晚于 UI 启动时用）。只能在归属
 * 任务调用；返回 true 表示 SD 兜底已就绪。 */

bool zh_font_retry_ui(void)
{
  if (g_ui_pid == 0 || getpid() != (pid_t)g_ui_pid)
    {
      return false;
    }

  if (g_src == ZH_SRC_SD_FULL)
    {
      return true;
    }

  if (g_retry_left <= 0)
    {
      return false;
    }

  g_retry_left--;
  zh_font_init();
  return g_src == ZH_SRC_SD_FULL;
}

zh_font_src_t zh_font_src(void)
{
  return g_src;
}

const lv_font_t *zh_font_get(int size_px)
{
  /* 防御：未 init 先 get 也能用（拷贝一次 flash 子集，无 SD 兜底） */

  zh_bind_main_fonts();

  switch (size_px)
    {
      case 12:
        return &s_main12;

      case 16:
        return &s_main16;

      case 19:
        return &s_main19;

      case 25:
        return &s_main25;

      default:
        break;
    }

  /* 容错：其它字号映射到最近的一档，绝不返回 NULL
   * （历史代码里还有 20/24 的调用点）。 */

  if (size_px <= 12)
    {
      return &s_main12;
    }

  if (size_px <= 16)
    {
      return &s_main16;
    }

  if (size_px <= 19)
    {
      return &s_main19;
    }

  return &s_main25;
}

int zh_font_count(int size_px)
{
  if (g_src == ZH_SRC_SD_FULL)
    {
      zh_sd_font_t *f = NULL;

      switch (size_px)
        {
          case 12:
          case 16:
            f = &s_sd_font16;
            break;

          case 19:
            f = &s_sd_font20;
            break;

          case 25:
            f = &s_sd_font24;
            break;

          default:
            return 0;
        }

      return (int)f->count;
    }

  return 648;   /* flash 子集（三套相同） */
}

bool zh_font_has_glyph(const lv_font_t *font, uint32_t letter)
{
  lv_font_glyph_dsc_t dsc;

  if (font == NULL)
    {
      return false;
    }

  return lv_font_get_glyph_dsc(font, &dsc, letter, letter);
}

/* 文本净化（方案说明见头文件）：把渲染不出来的字符剔掉。
 * 只丢"本来就会变方框"的字符，正常内容（ASCII、GB2312 汉字、
 * 标点、18 个内置 emoji）一字不动。 */

static bool zh_is_zero_width(uint32_t cp)
{
  /* 变体选择符 U+FE0E/0F、零宽连接/非连接 U+200D/200C、
   * 键帽组合 U+20E3、变音符区 U+FE00-FE0F、ZWSP 等：
   * 它们本身没有字形，留着就是多画一个方框。 */

  if (cp >= 0xFE00 && cp <= 0xFE0F)
    {
      return true;
    }

  switch (cp)
    {
      case 0x200B: case 0x200C: case 0x200D: case 0x20E3:
      case 0xFEFF:
        return true;

      default:
        return false;
    }
}

void zh_text_sanitize(char *dst, size_t dstsz, const char *src)
{
  const lv_font_t *font;
  size_t o = 0;

  if (dst == NULL || dstsz == 0)
    {
      return;
    }

  if (src == NULL)
    {
      dst[0] = '\0';
      return;
    }

  font = zh_font_get(16);

  while (*src != '\0' && o + 1 < dstsz)
    {
      int len = 0;
      uint32_t cp = zh_utf8_decode(src, &len);
      int k;

      if (len <= 0)
        {
          break;
        }

      if (cp >= 0x20 && (zh_is_zero_width(cp) ||
                         (font != NULL && !zh_font_has_glyph(font, cp))))
        {
          src += len;   /* 无字形：丢掉（保留控制字符如换行） */
          continue;
        }

      for (k = 0; k < len && o + 1 < dstsz; k++)
        {
          dst[o++] = src[k];
        }

      src += len;
    }

  dst[o] = '\0';
}

void zh_label_set_text_safe(lv_obj_t *label, const char *text)
{
  size_t n;
  char *tmp;

  if (label == NULL)
    {
      return;
    }

  if (text == NULL)
    {
      lv_label_set_text(label, "");
      return;
    }

  n = strlen(text) + 1;
  tmp = lv_malloc(n);

  if (tmp == NULL)
    {
      lv_label_set_text(label, text);
      return;
    }

  zh_text_sanitize(tmp, n, text);
  lv_label_set_text(label, tmp);
  lv_free(tmp);
}

int zh_font_check_text(int size_px, const char *text)
{
  const lv_font_t *font;
  int miss = 0;

  if (text == NULL || text[0] == '\0')
    {
      return 0;
    }

  font = zh_font_get(size_px);
  if (font == NULL)
    {
      return 0;
    }

  while (*text != '\0')
    {
      int len;
      uint32_t cp = zh_utf8_decode(text, &len);

      if (cp != 0 && !zh_font_has_glyph(font, cp))
        {
          miss++;
        }

      text += len;
    }

  return miss;
}


/****************************************************************************
 * 调试诊断（2026-09-10）：把"屏幕方框"定位到具体字与具体兜底层级
 ****************************************************************************/

/* 逐字符打印：主字体 / SD兜底 / 符号emoji / 缺（方框） */

void zh_font_trace_text(int size_px, const char *text)
{
  const lv_font_t *font = zh_font_get(size_px);

  if (font == NULL || text == NULL)
    {
      return;
    }

  while (*text != '\0')
    {
      lv_font_glyph_dsc_t dsc;
      const char *who = "缺(方框)";
      int len;
      uint32_t cp = zh_utf8_decode(text, &len);

      memset(&dsc, 0, sizeof(dsc));

      if (cp != 0 && lv_font_get_glyph_dsc(font, &dsc, cp, 0))
        {
          if (dsc.resolved_font == font)
            {
              who = "主字体";
            }
          else if (dsc.resolved_font == &s_sd_lvfont16 ||
                   dsc.resolved_font == &s_sd_lvfont20 ||
                   dsc.resolved_font == &s_sd_lvfont24)
            {
              who = "SD兜底";
            }
          else if (dsc.resolved_font == &s_icon_s ||
                   dsc.resolved_font == &s_icon_m ||
                   dsc.resolved_font == &s_icon_l ||
                   dsc.resolved_font == &s_icon_xl)
            {
              who = "符号emoji";
            }
          else
            {
              who = "其它字体";
            }
        }

      printf("[Font] U+%04X %.*s -> %s (box=%dx%d adv=%u)\n",
             (unsigned)cp, len, text, who, (int)dsc.box_w, (int)dsc.box_h,
             (unsigned)dsc.adv_w);
      text += len;
    }
}

/* 单码点：打印 SD 兜底查找全过程（每次 fseek/fread 的返回与 errno） */

void zh_font_diag_sd(uint32_t cp)
{
  zh_sd_font_t *f = &s_sd_font20;
  lv_font_glyph_dsc_t dsc;
  uint8_t b[ZH_SD_IDX_ENTRY];
  uint32_t lo = 0;
  uint32_t hi = f->count;
  int step = 0;

  printf("[Diag] src=%d main20.fb=%p sd20.fb=%p ud=%p fp=%p count=%u idx_off=%u\n",
         (int)g_src, (void *)s_main19.fallback, (void *)s_sd_lvfont20.fallback,
         (void *)s_sd_lvfont20.user_data, (void *)f->fp, (unsigned)f->count,
         (unsigned)f->idx_off);

  if (f->fp == NULL)
    {
      printf("[Diag] SD 文件句柄为空 -> 兜底不可用\n");
    }
  else
    {
      while (lo < hi && step < 20)
        {
          uint32_t mid = (lo + hi) / 2;
          long off = (long)ZH_SD_HEADER + (long)mid * ZH_SD_IDX_ENTRY;
          uint32_t u;
          int rc1;
          int e1;
          int e2;
          size_t rd;

          errno = 0;
          rc1 = fseek(f->fp, off, SEEK_SET);
          e1 = errno;
          errno = 0;
          rd = fread(b, 1, ZH_SD_IDX_ENTRY, f->fp);
          e2 = errno;

          u = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
              ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);

          printf("[Diag] #%d mid=%u off=%ld fseek=%d(e=%d) fread=%u(e=%d) u=%04X\n",
                 step, (unsigned)mid, off, rc1, e1, (unsigned)rd, e2,
                 (unsigned)u);
          step++;

          if (u < cp)
            {
              lo = mid + 1;
            }
          else if (u > cp)
            {
              hi = mid;
            }
          else
            {
              break;
            }
        }
    }

  memset(&dsc, 0, sizeof(dsc));
  printf("[Diag] U+%04X 20px整链=%d ph=%d resolved=%p\n", (unsigned)cp,
         (int)lv_font_get_glyph_dsc(&s_main19, &dsc, cp, 0),
         (int)dsc.is_placeholder, (void *)dsc.resolved_font);

  memset(&dsc, 0, sizeof(dsc));
  printf("[Diag] U+%04X 直查SD=%d ph=%d resolved=%p\n", (unsigned)cp,
         (int)lv_font_get_glyph_dsc(&s_sd_lvfont20, &dsc, cp, 0),
         (int)dsc.is_placeholder, (void *)dsc.resolved_font);
}

uint32_t zh_font_utf8_first(const char *text)
{
  int len = 0;

  if (text == NULL)
    {
      return 0;
    }

  return zh_utf8_decode(text, &len);
}

