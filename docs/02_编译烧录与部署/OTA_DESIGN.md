# OTA 升级设计（otadata + 自动回退）

## 架构：A/B 轮换，otadata 驱动

两个等价 Slot 轮换使用。**启动决策的唯一事实来源是 otadata 分区**
（第二级 bootloader 每次复位时读取），没有 KVDB/bootctl 记账层。

### 分区规划（16MB flash）

```
0x000000  bootloader.bin (esp-idf, 0x0)
0x008000  partition-table.bin
0x00D000  otadata（8KB：两份 esp_ota_select_entry_t 副本 @0 和 @0x1000）
0x010000  ota_0（/dev/ota0, 2MB）槽 A
0x210000  ota_1（/dev/ota1, 2MB）槽 B
0x410000  storage（littlefs + KVDB）
```

### otadata 条目

```
struct esp_ota_select_entry_t {
  uint32_t ota_seq;        // 递增序号；槽位 = (seq - 1) % 2
  uint8_t  seq_label[20];
  uint32_t ota_state;      // 0=NEW 1=PENDING_VERIFY 2=VALID 3=INVALID 4=ABORTED
  uint32_t crc;            // esp_rom_crc32_le(UINT32_MAX,&seq,4)，内部 init/结果各取反一次
};
```

### 升级流程（plant ota check）

```
1. 查 /version.json → 版本/硬件ID/大小校验
2. 目标槽 = otadata 推导的"非当前启动槽"（ota_get_active_slot）
3. find_mtddriver 直连 MTD（绕过 BCH/FTL 代理），只擦需要写的块
4. HTTP 下载 → 字节数核对 → SHA256 读回校验
5. 写 otadata：只写"非活动副本"，新条目 state=NEW、seq 递增映射到目标槽
   （活动副本保留旧条目作为回退参照）
6. 重启
```

### 自动回退（PENDING_VERIFY 机制）

需要 bootloader 以 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` 编译：

| 时机 | 状态 | 结果 |
|------|------|------|
| 写 otadata 后 | 新槽 NEW | 重启 |
| 首次启动新槽 | bootloader 把 NEW → PENDING_VERIFY | 启动新槽 |
| 新固件正常 | `plant ota confirm` → 标 VALID | 持续启动新槽 |
| 新固件崩溃/未确认重启 | bootloader 把 PENDING_VERIFY → ABORTED | **回退到旧槽（最高 seq 的 VALID 条目）** |

关键点：
- **只写非活动副本**：活动副本保留旧 VALID 条目作为回退目标。若两份都写成
  新 seq，崩溃后 bootloader 会从另一份 NEW 副本再次选中坏镜像 → 无限重启，
  而不是回退。
- `ota_confirm` 镜像 esp_ota_mark_app_valid_cancel_rollback()：把当前启动
  条目标为 VALID（CRC 只覆盖 seq，改 state 不失效）。
- 写槽防护：目标永远是"非当前启动槽"，写当前槽 = 擦掉正在执行的代码。

### 槽位交替

seq 驱动：`seq=1→ota0, 2→ota1, 3→ota0, 4→ota1, …`。当前 active=ota1 时，
下一次升级自动写 ota0（seq=3）。

### 关键坑（勿再踩）

1. 不要 `write-flash 0x0 nuttx.bin`（legacy 单镜像压住 ota_0）。
2. 不要写当前启动槽（ota.c 有硬防护）。
3. 不要用 `open()+write()` 写槽位（BCH/FTL 代理 → 每小写整块擦写，卡死）。
4. 不要依赖 KVDB/bootctl 判断启动槽（已实测漂移，不影响启动）。
