# OTA 工作日记（完整排查记录）

> 项目：ESP32-S3-BOX (OpenVela/NuttX) 固件 OTA 升级
> 时间跨度：V1.0.0 → 1.1.0 升级卡死排查 → 标准 A/B 部署 → otadata 驱动 + 自动回退
> 最后更新：2025-08-17

---

## 一、问题起点：OTA 升级卡死在 `Progress: 51506 / 1178204 bytes`

**现象**：设备 V1.0.0 升级 1.1.0，HTTP 下载到 51506/1178204 字节后不再前进（像死机）。

**初始排查方向**（用户策略）：`ota_pre_erase()` 用 `MTDIOC_GEOMETRY` / `MTDIOC_ERASESECTORS` 预擦除、3 步下载。

### 根因 1：BCH/FTL 字符设备代理（CONFIG_BCH=y）
`open("/dev/otaX")` 拿到的是包了 BCH+FTL 的字符设备代理。每次小写入（1KB recv buffer）都触发**整块 4KB 的读-擦-写循环**，擦除 2MB 槽位要 512 次擦写，慢到看起来像卡死。

**修复**：`find_mtddriver()` 直连 MTD（`mtd->u.i_mtd`），绕开 BCH/FTL 代理；只擦将要写入的块（按 `geo.erasesize` 逐块 `MTD_ERASE`），`MTD_WRITE` 直接字节编程。

### 根因 2：legacy 单镜像压住 ota_0
旧部署把整份 `nuttx.bin` 烧在 **0x0**（无 bootloader/分区表/otadata）。镜像从 0x0 到约 0x130000，`.text` 在 0x50000 —— **运行中的代码就在 ota_0（0x10000 起）区域里**。OTA 一擦 ota_0 就把正在执行的代码擦掉 → 必死。

**修复**：改为标准 ESP32-S3 A/B 三件套部署，且**下载目标永远 = otadata 推导出的"非当前启动槽"**（`ota.c` 里有硬防护，拒绝写 `g_ota_ctx.active_slot`）。

---

## 二、架构选型过程

1. 最初尝试在用户坚持的 bootctl/KVDB 框架内做（`bootctl_update` / `bootctl_done` / `bootctl_success` 记账）。
2. 实测发现 **KVDB/bootctl 不影响启动**：bootloader 只读 otadata 决定启动槽，KVDB 的 persist.boot.* 只是没人读的记账。曾因依赖它导致"升级永远不生效"。
3. 选定**标准 A/B bootloader 部署（方案 A）**：esp-idf bootloader@0x0 + 分区表@0x8000 + otadata@0xD000 + ota_0/ota_1。
4. 最终（本次会话）删掉 bootctl/KVDB 记账层，**otadata 成为唯一事实来源**，并加上**自动回退（PENDING_VERIFY）**。

---

## 三、部署与配置（标准 A/B，16MB flash）

### 分区表 `partition_table.csv`
| 分区 | 地址 | 大小 | 说明 |
|------|------|------|------|
| nvs | 0x9000 | 16KB | 未使用 |
| otadata | 0xD000 | 8KB | 启动槽元数据（/dev/otadata，两份副本 @0/@0x1000） |
| phy_init | 0xF000 | 4KB | 未使用 |
| ota_0 | 0x10000 | 2MB | 槽 A（/dev/ota0） |
| ota_1 | 0x210000 | 2MB | 槽 B（/dev/ota1） |
| storage | 0x410000 | ~4MB | littlefs(/data) + KVDB |

### defconfig 关键值（vendor/espressif/.../openvela/defconfig）
```
CONFIG_ESP32S3_STORAGE_MTD_OFFSET=0x410000   # 必须！早期 0x250000 落在 ota_1 内，OTA 会毁掉存储
CONFIG_ESP32S3_STORAGE_MTD_SIZE=0x3F0000
CONFIG_ESP32S3_OTA_PRIMARY_SLOT_OFFSET=0x10000
CONFIG_ESP32S3_OTA_SECONDARY_SLOT_OFFSET=0x210000
CONFIG_ESP32S3_OTA_SLOT_SIZE=0x200000
```
> 槽位必须是 2MB：`olddefconfig` 曾把它们重置回 1MB 默认值导致 `find_mtddriver /dev/ota1` 报 -2。

---

## 四、踩坑全记录（按时间）

### 1. A/B 三件套烧录后启动卡死
- 现象：bootloader + 分区表 + app@0x10000 烧好后，复位卡住不启动。
- 根因：kernel 用 SIMPLE_BOOT 格式构建时，从槽位加载会把 MMU 按 `PRIMARY_SLOT_OFFSET=0` 重映射，与 0x10000 槽位不符。
- 修复：改用 **LEGACY app 格式**（镜像自带 5 段 header）。在 `nuttx/boards/xtensa/esp32s3/esp32s3-box/Kconfig` 加隐藏项 `ESP32S3_BOX_USE_LEGACY_APP_FORMAT`（`default y`、`select ESP32S3_APP_FORMAT_LEGACY`），`nuttx/arch/xtensa/src/esp32s3/Kconfig` 的 LEGACY 加 `select ESP32S3_HAVE_OTA_PARTITION`。**不需要改 defconfig**。

### 2. `find_mtddriver /dev/ota1 failed: -2`
- 根因：`HAVE_OTA_PARTITION` 原本只在 MCUBOOT 下被 select，A/B 布局下没定义 ota 槽位设备。
- 修复：LEGACY 格式 select 它（见上）。

### 3. 链接 undefined refs（bootctl_main / property_* / iperf2_main / cJSON / unqlite）
- 根因：`apps/frameworks -> ../frameworks` 是**符号链接**，`find apps` 不带 `-L` 搜不到，导致 frameworks 里陈旧的 `.built`/`.o` 没被清理，引用已删源码的符号。
- 修复：按组件删除陈旧构建产物后重编。

### 4. esp-hal-3rdparty 被删 + 克隆超时（最惊险）
- 现象：一次 `distclean` 里 `DELDIR chip/esp-hal-3rdparty` 把 HAL 整个删掉；用 kgithub 镜像克隆又 135s 超时。
- 修复：
  - 删除 `nuttx/arch/xtensa/src/esp32s3/Make.defs` 里的 `distclean:: $(call DELDIR,chip/esp-hal-3rdparty)`（**今后不会再被 distclean 删**）；
  - `ESP_HAL_3RDPARTY_URL` 默认改为 `https://ghfast.top/https://github.com/espressif/esp-hal-3rdparty.git`；
  - 用 ghfast.top 恢复仓库 + 5 个子模块（mbedtls, esp_phy/lib, esp_wifi/lib, bt/controller/lib_esp32c3_family, esp_coex/lib）+ spinlock sed 修复（`LOCK_INITIALIZER_UNLOCKED 0 → SP_UNLOCKED`）。
- 用户 git 全局把 github→kgithub 的改写也会干扰，`.gitmodules` 直接用 ghfast 直连 URL 规避。

### 5. KVDB 漂移实证（促成架构决策）
设备从 ota1 启动（otadata seq=2）后：
```
getprop persist.boot.slot_a.active = true   # KVDB 说 ota0 是 active
bootctl slot → /dev/ota0                    # 记账层与事实不符
[OTA] Init: active=/dev/ota1                # 实际启动槽
```
结论：**KVDB/bootctl 是装饰，启动只认 otadata**。这也是本次删除它的直接证据。

---

## 五、最终调试：CRC 三次修正 + 复位机制五连坑（最烧脑的一段）

### 1. otadata CRC：三次才对齐（seq=1 的 CRC 三种算法值）

| 尝试 | 算法 | seq=1 的 CRC | 结果 |
|------|------|-------------|------|
| 原版 | zlib 式（init 0xFFFFFFFF + 最终 XOR） | 0x99F8B879 | ❌ |
| 第 1 次修 | 裸值（去掉最终 XOR） | 0x66074786 | ❌ |
| **正确** | `esp_rom_crc32_le(UINT32_MAX,&seq,4)`：**init 和结果各取反一次**（等效 init=0 起算、结果 `~crc`） | **0x4743989A** | ✅ |

- 教训：bootloader 校验用 `bootloader_common_ota_select_crc()` = `esp_rom_crc32_le(UINT32_MAX, &seq, 4)`。该 ROM 函数内部先 `~init` 再算再 `~结果`——不要想当然按 zlib 写。
- 症状：`active=(uninitialized)`——bootloader 初始化的条目 CRC 对不上，被 `ota_otadata_selectable()` 判为无效。
- 附带：这也暴露了**之前"从 ota1 启动成功"是假象**——旧代码只看 seq 不看 CRC，bootloader 实际因 CRC 错回退到 ota0，app 却自称 active=ota1。

### 2. "ota1 起不来"是假象（手动复位两次 = ABORTED）

- 现象：check 后 status 显示 st1=4 (ABORTED)，误判 ota1 启动失败。
- 真相：**`up_systemreset()` 不工作，用户手动按复位键且按了两次** → boot#1: NEW→PENDING → ota1 **其实起来了** → boot#2（第二次按键）: PENDING→ABORTED → 回退 ota0。**回退机制反而被证明工作正常。**
- 关键实验：手工写 otadata（seq2 VALID）让 bootloader 直接启动 ota1 → **ota1 稳定运行**（隔离实验证明 ota1 本身能启动）。

### 3. up_systemreset 五连坑（ESP32-S3 软件复位为什么难）

esp-idf 的 `esp_restart_noos()`（`esp32s3/system_internal.c`）复位前要做一堆清理；NuttX 原版只写一个寄存器，所以一路踩坑：

| 版本 | 加了什么 | 结果 |
|------|---------|------|
| 原版 | 裸写 `RTC_CNTL_SW_SYS_RST` | 设备不重启（或复位后 flash 死） |
| v2 | + RTC WDT 武装（1s 超时兜底） | 还是不重启 |
| v3 | + GPIO_FUNC0-5 恢复(0x30) + 关 ICache/DCache | 还是不行，但 **USB 重枚举 = 芯片确实复位了** |
| v4 | + CPU 时钟回 XTAL（`esp32s3_rtc_update_to_xtal`） | 打出 RST-1/2/3 后 minicom 卡死 |
| **v5 ✅** | + **外设复位脉冲（SYSTEM_SPI01_RST 等）** + 删关缓存后的打印 | **rst:0x3 (RTC_SW_SYS_RST)，flash 正常读，ota1 启动！** |

**每个坑的根因**：
1. **裸 SW_SYS_RST 无效/复位后 flash 死**：软件复位保留 RTC 域（PLL、时钟配置、SPI 控制器状态）。
2. **CPU 时钟**：NuttX 把 CPU 跑在 PLL/240MHz；软件复位不清 RTC 域 → ROM 启动时代钟不匹配 → 挂死。必须 `esp32s3_rtc_update_to_xtal(40,1)` 回 XTAL（**要在关缓存前做**——它调用非 IRAM 代码）。
3. **SPI01（flash 控制器）**：OTA 下载写 1.1MB 后 SPI01 停在写配置状态 → 复位后 ROM 读 flash 全 0xFF（"invalid header: 0xffffffff"）。必须**复位脉冲**：`SYSTEM_PERIP_RST_EN0_REG` 置 `SYSTEM_SPI01_RST|SYSTEM_TIMERS_RST|SYSTEM_UART_RST|SYSTEM_SYSTIMER_RST` 再清 0。
4. **关缓存后不能碰任何 flash 数据**：连 `ets_printf` 的格式字符串都在 flash DROM——DCache 关掉后取格式串直接挂死（minicom 卡死的原因）。调试打印只能放在关缓存之前。
5. **波特率乱码**：CPU 时钟切到 XTAL 后 APB 变化 → UART 波特率变 → 调试打印变乱码。是假象，复位后 ROM 重新配置 UART 就正常。
6. **手动复位键为什么总是有效**：EN 键 = 全硬件复位，清掉所有 RTC 域状态；软件复位不清，所以一路踩坑。

**最终 up_systemreset 序列**（全部在 IRAM，镜像 `esp_restart_noos`）：
```
禁中断 → GPIO_FUNC0-5 恢复(0x30) → CPU 时钟回 XTAL(esp32s3_rtc_update_to_xtal)
→ 关 ICache/DCache → 武装 RTC WDT(1s 兜底) → 外设复位脉冲(SPI01 等)
→ SW_SYS_RST + software_reset_cpu(0) → for(;;)
```

### 4. 验证通过的关键证据（2025-08-17）

```
rst:0x3 (RTC_SW_SYS_RST), boot:0x2a (SPI_FAST_FLASH_BOOT)   ← SW_SYS_RST 生效
mode:DIO, clock div:1                                        ← flash 正常读
I (334) boot: Loaded app from partition at offset 0x210000   ← 从 ota1 启动
otadata: seq0=1 st0=2 | seq1=2 st1=1 (PENDING_VERIFY)       ← 切槽成功，无二次复位
active=/dev/ota1
```

---

## 六、新架构：otadata 唯一事实来源 + 自动回退（本次会话完成）

### 1. 删除 bootctl/KVDB 记账层（省 RAM）
- defconfig 删除 `CONFIG_UTILS_BOOTCTL=y` + SLOT_A/SLOT_B（bootctl 命令不再编译，省 4096 栈 + 代码）。
- `ota.c` 删除全部 `bootctl_*()` 调用、`<bootctl/bootctl.h>`、`<cutils/properties.h>`；`ota_init` 不再回退到 `bootctl_active()`。
- `plant ota status` 只显示 otadata（两份副本 seq+state + active/target + state 含义表）。
- KVDB 子系统本体保留（`system/init`、getprop 等系统组件还在用），只删 OTA 记账用途。
- 删除 `OTA_TASKS.md`（旧 bootctl 阶段任务清单）、重写 `OTA_DESIGN.md`。

### 2. otadata 读写重构（镜像 bootloader 语义）
```c
ota_otadata_selectable(e)  // seq!=0xFFFFFFFF && state 非 INVALID/ABORTED && CRC 匹配
ota_otadata_active(e0,e1,&idx)  // 取可启动副本中 seq 最大者（与 bootloader 选择一致）
ota_get_active_slot()      // 由选中条目推导 slot = (seq-1)%2
```

### 3. 自动回退（PENDING_VERIFY 机制）
**bootloader 侧**（重编，`esp32s3-bl/hello_world`）：
```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y   # 已加入 sdkconfig.defaults，重编并验证 Chip ID=9
```
**app 侧**（`ota.c`）：
- `ota_write_otadata()`：**只写"非活动副本"**，新条目 `state=NEW(0x00)`、seq 递增映射目标槽。
- `ota_confirm()` / `ota_mark_current_valid()`：把当前启动条目标 `VALID(0x02)`（镜像 `esp_ota_mark_app_valid_cancel_rollback`，CRC 只覆盖 seq，改 state 不失效）。

**回退流程**：
```
写 otadata (NEW) → 重启
→ bootloader 把 NEW 改写为 PENDING_VERIFY(0x01)，启动新槽
→ 新固件正常：plant ota confirm → VALID，持续使用
→ 新固件崩溃/未确认：下次启动 bootloader 把 PENDING_VERIFY → ABORTED(0x04)
   → 回退到保留的旧 VALID 条目（旧槽）
```

### 4. 关键设计决策（为什么）
| 决策 | 原因 |
|------|------|
| 往哪写只靠 otadata | 它是 bootloader 唯一读的状态，永不漂移；KVDB 已实测漂移 |
| 只写一份 otadata 副本 | 活动副本保留旧 VALID 条目作回退参照；两份都写新 seq → 崩溃后从另一份 NEW 副本再次选中坏镜像 → **无限重启而非回退** |
| state=NEW 而非 VALID | NEW 触发 bootloader 的 PENDING_VERIFY 流程，是回退的前提 |
| 排除 INVALID/ABORTED 条目 | 与 bootloader 的 `ota_select_valid` 一致，回退后 active 推导正确 |

---

## 七、本次修改文件清单

### apps/plant-companion/ota/ota.c（核心重写）
- 删 `<bootctl/bootctl.h>`、`<cutils/properties.h>`、全部 bootctl_* 调用
- 新增状态宏：NEW/PENDING_VERIFY/VALID/INVALID/ABORTED（与 esp_ota_img_states_t 一致）
- 新增 `ota_otadata_selectable()` / `ota_otadata_active()`（bootloader 选择逻辑镜像）
- `ota_otadata_read()` 改为返回完整条目（seq+state+crc）
- `ota_write_otadata()`：只写非活动副本 + state=NEW
- 新增 `ota_mark_current_valid()`；`ota_confirm()` 改为标 VALID
- `ota_print_boot_status()`：只显示 otadata 层

### apps/plant-companion/ota/ota.h
- 删 `OTA_ERR_BOOTCTL`；更新全部注释（confirm = 标 VALID/取消回退）

### nuttx/arch/xtensa/src/esp32s3/esp32s3_systemreset.c（复位修复）
- `up_systemreset()` 重写为 esp_restart_noos 完整镜像：禁中断 → GPIO_FUNC0-5 恢复(0x30) → CPU 时钟回 XTAL（`esp32s3_rtc_update_to_xtal`）→ 关 ICache/DCache → 武装 RTC WDT（STG0=RESET_SYSTEM 1s）→ 外设复位脉冲（`SYSTEM_SPI01_RST` 等）→ `SW_SYS_RST` + `software_reset_cpu(0)` → for(;;)
- 整个函数 `IRAM_ATTR`（关缓存后不能再从 flash 取指令）
- 调试期加过 `ets_printf("RST-n")` 进度打印（关缓存前的保留，关缓存后的会因格式串在 DROM 而挂死——已删）

### apps/plant-companion/main/app_main.c
- 帮助文本更新（confirm = Mark current slot VALID / cancel rollback）
- 调试命令 `plant ota reboot`（直接调 up_systemreset，绕开 OTA 流程隔离测试复位）——**最终交付前可删**

### apps/plant-companion/Makefile
- 删 bootctl/kvdb include 路径（`frameworks/system/ota`、`frameworks/system/utils/include`）

### apps/plant-companion/Kconfig
- `PLANT_OTA_UPDATE` 帮助文本去掉 bootctl/KVDB 要求

### vendor/.../esp32s3-box/configs/openvela/defconfig
- 删 `CONFIG_UTILS_BOOTCTL=y` + SLOT_A/B

### 文档
- `OTA_TASKS.md` 删除；`OTA_DESIGN.md` 重写为 otadata+回退设计；`CLAUDE.md` OTA 章节更新

### bootloader
- `esp32s3-bl/hello_world/sdkconfig.defaults` 加 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
- 重编并验证：Chip ID 9 (ESP32-S3)、16MB/80m/DIO、大小 20912B（原 20832B，含回退逻辑）
- 拷贝为 `nuttx/bootloader.bin`

---

## 八、编译产物与验证方法（编译由用户执行）

```bash
# 编译 app（nuttx/nuttx.bin）
source ~/openvela-venv/bin/activate
./build.sh vendor/espressif/boards/esp32s3/esp32s3-box/configs/openvela/
# 注意：删过 nuttx/.config 重新生成（备份在 /tmp/config.backup.*）；savedefconfig 会重写 defconfig

# 烧录三件套（16MB）
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 --before default-reset --after hard-reset erase_flash
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 nuttx/bootloader.bin  0x8000 nuttx/partition-table.bin  0x10000 nuttx/nuttx.bin
```

**板上验证清单**（2025-08-17 已全部通过）：
1. `plant ota status` → 两份 otadata seq+state、active/target（无 KVDB 行）。
2. `plant wifi <ssid> <pass>` → `plant ota check` → 下载 → 写 otadata(NEW) → **自动重启**（修复后无需手动！）。
3. 重启后 status → `active=目标槽, st=1 (PENDING_VERIFY)` ← 切槽成功（证据见第五节）。
4. `plant ota confirm` → `st=2 (VALID)` → 再重启 → 留在新槽。
5. 回退测试：check 后**不 confirm** 直接再复位 → PENDING→ABORTED → 回退旧槽（机制已多次实测）。
6. 服务器：`/home/vboxuser/ota-server/firmware/version.json` 版本需 > 设备版本；`./update_firmware.sh <版本>` 自动重算 size/sha256。

---

## 九、遗留事项 / 下一步

- [ ] 清理调试代码：`ota.c` 无调试残留；`esp32s3_systemreset.c` 的 RST-1/2/3 打印、`app_main.c` 的 `plant ota reboot` 命令（交付前删）
- [ ] 正式升级：bump `OTA_FW_VERSION_MINOR` → 重编 → `./update_firmware.sh` 更新服务器
- [ ] `frameworks/system/ota/bootctl/` 源码未删（上游共享代码，仅停用配置）；如需彻底移除可再议
- [ ] 掉电安全测试（写槽位中途断电 → 应从旧槽启动）
- [ ] 交接包（增量包/1GB 快照）在最终版本定稿后重新生成（当前含调试打印的旧构建）
