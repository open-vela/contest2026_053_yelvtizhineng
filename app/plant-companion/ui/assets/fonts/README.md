# 中文字体资源（lv_font_conv 生成）

本目录存放植小伴 UI 的中文字体子集与生成工具链。

## 字体文件

| 文件 | 字号 | 用途 | 大小 |
|---|---|---|---|
| `lv_font_plant_zh_16.c` | 16px | 正文（TP_FONT_BODY） | ~210KB |
| `lv_font_plant_zh_20.c` | 20px | 副标题/按钮（TP_FONT_TITLE） | ~293KB |
| `lv_font_plant_zh_24.c` | 24px | 大标题/大数字（TP_FONT_BIG） | ~397KB |

符号名：`lv_font_plant_zh_16` / `lv_font_plant_zh_20` / `lv_font_plant_zh_24`
（在 `ui/theme/theme_plant.h` 中 extern 声明，`Makefile` 已加入 CSRCS）

## 生成命令

```bash
# 依赖：node + lv_font_conv（本目录 node_modules 已装）
node node_modules/lv_font_conv/lv_font_conv.js \
  --no-compress --no-prefilter --bpp 4 \
  --size 16 --format lvgl \
  -o lv_font_plant_zh_16.c \
  --font NotoSansSC-Regular.otf \
  -r 0x20-0x7E,0x00B0,0x2014,0x3001,0x3002,0xFF01,0xFF0C,0xFF1A,0xFF1F \
  --symbols "$(cat symbols.txt)"
# size 换成 20/24 生成另两档
```

## 用字

- `symbols.txt`：UI 用字全集（UI_SPEC §5，约 130 个常用汉字 + 数字/标点）
- 字源：Noto Sans CJK SC（思源黑体简体），从 Google noto-cjk 仓库下载
- 新增文案时：把新汉字追加进 `symbols.txt`，重新跑上述命令即可

## 注意

- **emoji 不在此字体中**（思源黑体无 emoji 字形），UI 图标用 LVGL montserrat 符号或后续 PNG→C 素材
- 字体 C 数组约 900KB 进 flash（const），符合 2MB 槽位预算
- `NotoSansSC-Regular.otf`（16MB）仅构建期需要，可删除以省空间；`.c` 文件必须保留

## emoji 单色子集（2026-09-03 新增：修复 UI emoji 渲染方块）

| 文件 | 源 | 内容 |
|---|---|---|
| `lv_font_emoji_{16,20,24}.c` | `OpenMoji-black-glyf.ttf`（OFL 单色）| UI 用 16 个真 emoji：🌱🌿🎉🏅💧📈📊😊🥱😢🙏🛠⚠✅⭕🌡 |

- 角色：zh_font.c fallback 链尾（`zh 主字体 → [SD 汉字] → montserrat(符号) →
  emoji`）；`zh_font.c` `extern const lv_font_t lv_font_emoji_*`，Makefile 已加 CSRCS。
- 生成（改 UI emoji 后重跑；node + 本目录 lv_font_conv）：
  ```bash
  R="-r 0x1F331 -r 0x1F33F -r 0x1F389 -r 0x1F3C5 -r 0x1F4A7 -r 0x1F4C8 -r 0x1F4CA \
     -r 0x1F60A -r 0x1F622 -r 0x1F64F -r 0x1F6E0 -r 0x1F321 -r 0x1F971 \
     -r 0x26A0 -r 0x2705 -r 0x2B55"
  for SZ in 16 20 24; do
    node node_modules/lv_font_conv/lv_font_conv.js --font OpenMoji-black-glyf.ttf \
      $R --size $SZ --bpp 4 --format lvgl \
      --lv-font-name lv_font_emoji_$SZ --force-fast-kern-format \
      -o lv_font_emoji_$SZ.c
  done
  ```
- 坑：① OpenMoji 无 U+2713(✓) → 用 ✅；② emoji 别带 U+FE0F（"⚠️"→"⚠"，已清理，
  服务端文本若带回 VS16 显示前需剥）；③ 旧 `NotoEmoji-Regular.ttf` 是 HTML 假文件已删，
  勿再引用。

## 字体源文件（不入库）

为控制仓库体积，`NotoSansSC-Regular.otf` 与 `OpenMoji-black-glyf.ttf` **未随仓提交**
（`.c` 字模已生成并入库，编译不需要源字体）。需要重新生成字模时自行下载：

| 文件 | 来源 |
|------|------|
| `NotoSansSC-Regular.otf` | Google [noto-cjk](https://github.com/notofonts/noto-cjk) 仓库（Sans/SubsetOTF/SC） |
| `OpenMoji-black-glyf.ttf` | [OpenMoji](https://github.com/hfg-gmuend/openmoji) 的 black-glyf 版本（OFL 授权） |

下载后放回本目录，按上文命令重跑 `lv_font_conv` 即可。
