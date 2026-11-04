# SD 卡架构方案

> 2025-08-27 更新：按板级原理图修正引脚/设备/挂载点，新增 `CONFIG_PLANT_SD_CARD` 开关。

## 当前状态（已实现，待板级验证）

- `sd_card_init()` - 开机自动挂载（失败不阻塞启动，非致命）
- `sd_card_mount()` / `sd_card_umount()` - 手动挂载/卸载
- `sd_card_status()` - 查询是否已挂载（statfs 判 f_type == MSDOS_SUPER_MAGIC）
- `sd_card_test()` - 写入/读取/验证回环测试（已挂载则复用，测试后恢复原状态）
- NSH 命令：`plant sd status / mount / test / umount`

### 设备路径（已核实）

- 块设备：`/dev/mmcsd1` —— `esp32s3_bringup.c → board_sdmmc_initialize() →
  sdio_initialize(1) → mmcsd_slotinitialize(1, sdio)`，minor=1 即 /dev/mmcsd1
  （旧文档写的 /dev/mmcsd0 是错的）
- 挂载点：`/mnt/sd`（vfat）—— 旧挂载点 /tmp/sd 已废弃
- 挂载失败错误区分：
  - `ENODEV/ENOENT/ENOTBLK` → 卡未插（板子无 CD 引脚，卡必须开机前插好）
  - `EINVAL` → 有卡但无 FAT 文件系统（提示 `mkfatfs /dev/mmcsd1` 格式化）

## 硬件配置（以原理图为准）

### GPIO 引脚（1-bit 模式，GPIO 矩阵路由）

| 信号 | GPIO | 方向 | 说明 |
|------|------|------|------|
| CMD | IO0  | 双向 | 命令/响应（原理图 R63 0Ω 串接 + R58 10K 上拉） |
| CLK | IO43 | 输出 | 时钟 |
| D0  | IO44 | 双向 | 数据线 |

> ⚠️ **IO0 同时是 BOOT 按键**（低有效）：运行中按 BOOT 会拉低 SD_CMD 导致 SD 通信错误；
> 上电时按住 BOOT 会进下载模式。原理图有 10K 上拉缓解，属硬件特性，代码无法避免。
> ⚠️ 旧文档（CLAUDE.md 曾写 SD_CMD=IO48、架构文档曾写 39/41/40）均为错误信息；
> 39/41/40 只是 NuttX Kconfig 默认值，从未生效。

### Kconfig 配置项

```
# 芯片级（vendor/.../configs/openvela/defconfig 已配）
CONFIG_ESP32S3_SDMMC=y
CONFIG_ESP32S3_SDMMC_CMD=0      # IO0
CONFIG_ESP32S3_SDMMC_CLK=43     # IO43
CONFIG_ESP32S3_SDMMC_D0=44      # IO44
CONFIG_SDIO_WIDTH_D1_ONLY=y     # 1-bit 模式（只用 D0）

# 文件系统（defconfig 已配）
CONFIG_FS_FAT=y
CONFIG_FAT_LFN=y                # 长文件名
CONFIG_FAT_LFN_UTF8=y           # UTF-8 中文文件名

# App 级开关（apps/plant-companion/Kconfig，defconfig 已启用）
CONFIG_PLANT_SD_CARD=y          # 依赖 ESP32S3_SDMMC
```

## 启动流程

```
上电（卡必须在此时已插入）
  ↓
esp32s3_bringup.c
  ↓
board_sdmmc_initialize()
  ↓
sdio_initialize(1)              # 初始化 SDIO slot 1
  ↓
配置 GPIO 矩阵（CMD=IO0/CLK=IO43/D0=IO44，CMD/D0 上拉）
  ↓
mmcsd_slotinitialize(1, sdio)   # 注册 /dev/mmcsd1
  ↓
plant 启动 → sd_card_init() → mount("/dev/mmcsd1", "/mnt/sd", "vfat")
  ↓
plant sd test / 应用读写 /mnt/sd
```

## 已知限制（硬件决定）

1. **无卡检测引脚**：驱动把 card detect 接到 `GPIO_MATRIX_CONST_ZERO_INPUT`
   （esp32s3_sdmmc.c 初始化），始终报"有卡" → **卡必须开机前插好，无热插拔**。
2. **IO0 复用 BOOT**：见上，运行中按 BOOT 键破坏 SD 通信。
3. **无电源管理**：无法对卡槽单独复位/断电。
4. **未接 8 线 PSRAM 数据线**：IO35/36/37 被 8x PSRAM 占用（原理图标注 NA），
   与 SD 无关，仅说明可用 GPIO 有限。

## 已知坑：设备端 mkfatfs 格式化大卡必败（2025-08-27 实机验证）

- **现象**：`mkfatfs /dev/mmcsd1` 报 `mkfatfs failed: 23`（errno 23 = ENFILE，但
  此处是 mkfatfs **自己的错误码**，不是"文件描述符耗尽"）。
- **根因**（apps/fsutils/mkfatfs/configfat.c）：自动模式（FAT_FORMAT_INITIALIZER
  `ff_fattype=0`）**只尝试 FAT12/FAT16**，FAT32 必须显式 `-F 32`；
  FAT16 上限 = 65520 簇 × 最大 128 扇区/簇(64KB) ≈ **4.2GB**。
  卡 ≥ ~4GB 时所有簇大小都试完仍无解 → configfat.c:868 `return -ENFILE`。
- **与挂载不矛盾**：mount 只是**读取**卡上已有 FAT32，mkfatfs 要**计算**新 FAT
  几何参数 → 大卡自动模式必然失败。卡可挂载读写 = 不需要格式化。
- **决策（2025-08-27）**：不在设备端格式化。格式化用读卡器在 Windows 上做
  （右键格式化选 FAT32 即可），NuttX 侧 vfat 直接兼容。
- 若确需设备端格式化：`mkfatfs -F 32 /dev/mmcsd1`（⚠️ 会清空卡上数据）。

## 后续集成方向（本次未做，需用户拍板）

- 照片（SD 卡）入库日记（3D-2）：拍照 JPEG 存 /mnt/sd/photos/，日记时间线引用
- 任务/日记持久化迁 SD：record_service 的 /data littlefs 迁 /mnt/sd
  （注意：SD 是 FAT，不支持 littlefs 的原子性假设，需评估）
- 植物百科 JSON 放 SD：大文件不占 SPI flash
