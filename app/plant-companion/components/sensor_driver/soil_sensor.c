/****************************************************************************
 * soil_sensor.c — 土壤传感器驱动实现
 *
 * RS485 Modbus RTU 通信，8参数土壤传感器
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>

#include "soil_sensor.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MODBUS_CMD_SIZE    8      /* Modbus 命令长度 */
#define MODBUS_RESP_SIZE   21     /* Modbus 响应长度（8寄存器×2 + 5头尾） */

#define REG_TEMP           0      /* 寄存器0：温度 */
#define REG_MOISTURE       1      /* 寄存器1：湿度 */
#define REG_EC             2      /* 寄存器2：电导率 */
#define REG_SALT           3      /* 寄存器3：盐分 */
#define REG_NITROGEN       4      /* 寄存器4：氮 */
#define REG_PHOSPHORUS     5      /* 寄存器5：磷 */
#define REG_POTASSIUM      6      /* 寄存器6：钾 */
#define REG_PH             7      /* 寄存器7：pH */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* ⚠️ 不缓存 fd：NuttX 的 fd 表按**任务组**隔离（sched_getfiles 取当前任务
 * 的 group），界面进程打开的 fd 在 NSH 手动命令的任务组里无效。旧实现把
 * fd 存全局 → 手动 `plant soil read` 必然 EBADF 秒失败（2026-09-09 根因）。
 * 改成"谁读谁 open、读完 close"，g_uart_lock 把界面轮询线程与手动命令
 * 串行化（同一串口不能并发收发）。 */

static char g_dev[32] = SOIL_SENSOR_DEV_DEFAULT;
static int g_baudrate = SOIL_SENSOR_BAUD_DEFAULT;
static pthread_mutex_t g_uart_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_paused;                    /* true=摄像头占用 IO42/40，暂停收发 */

/* Modbus 命令缓冲区 */

static uint8_t g_modbus_cmd[MODBUS_CMD_SIZE];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: soil_sensor_crc16
 *
 * Description:
 *   计算 Modbus CRC16（多项式 0xA001）
 *
 ****************************************************************************/

static uint16_t soil_sensor_crc16(const uint8_t *buf, size_t len)
{
  uint16_t crc = 0xFFFF;
  size_t i;
  int j;

  for (i = 0; i < len; i++)
    {
      crc ^= buf[i];
      for (j = 0; j < 8; j++)
        {
          if (crc & 1)
            {
              crc = (crc >> 1) ^ 0xA001;
            }
          else
            {
              crc >>= 1;
            }
        }
    }

  return crc;
}

/****************************************************************************
 * Name: soil_sensor_build_command
 *
 * Description:
 *   构建 Modbus 读寄存器命令
 *
 ****************************************************************************/

static void soil_sensor_build_command(uint8_t addr, uint16_t reg,
                                      uint16_t count)
{
  uint16_t crc;

  g_modbus_cmd[0] = addr;                   /* 设备地址 */
  g_modbus_cmd[1] = 0x03;                   /* 功能码：读保持寄存器 */
  g_modbus_cmd[2] = (reg >> 8) & 0xFF;     /* 起始地址高字节 */
  g_modbus_cmd[3] = reg & 0xFF;             /* 起始地址低字节 */
  g_modbus_cmd[4] = (count >> 8) & 0xFF;   /* 数量高字节 */
  g_modbus_cmd[5] = count & 0xFF;           /* 数量低字节 */

  crc = soil_sensor_crc16(g_modbus_cmd, 6);
  g_modbus_cmd[6] = crc & 0xFF;             /* CRC 低字节 */
  g_modbus_cmd[7] = (crc >> 8) & 0xFF;     /* CRC 高字节 */
}

/****************************************************************************
 * Name: soil_sensor_configure_uart
 *
 * Description:
 *   配置 UART 参数
 *
 ****************************************************************************/

static int soil_sensor_configure_uart(int fd, int baudrate)
{
  struct termios tty;

  /* 清零整个结构，避免残留脏数据 */

  memset(&tty, 0, sizeof(struct termios));

  /* 8N1，启用接收 */

  tty.c_cflag = CS8 | CREAD | CLOCAL;

  /* 输入配置：忽略 break，启用奇偶校验检测 */

  tty.c_iflag = IGNBRK | INPCK;

  /* 波特率【最后】设：c_cflag 是整体赋值，先设会被这里的 = 覆盖掉，
   * 结果波特率落回默认值 → 向 9600 的传感器发乱码 → 永远无应答。
   * 顺序对照 apps/modbus/nuttx/portserial.c（项目已验证范式）。 */

  if (cfsetispeed(&tty, baudrate) != 0)
    {
      printf("[SOIL] Failed to set baudrate: %d\n", errno);
      return -1;
    }

  /* 不设置 VMIN/VTIME，由 select() 控制超时 */

  if (tcsetattr(fd, TCSANOW, &tty) < 0)
    {
      printf("[SOIL] Failed to set UART attrs: %d\n", errno);
      return -1;
    }

  return 0;
}

/****************************************************************************
 * Name: soil_sensor_send_command
 *
 * Description:
 *   发送 Modbus 命令
 *
 ****************************************************************************/

static int soil_sensor_send_command(int fd)
{
  int ret;

  ret = write(fd, g_modbus_cmd, MODBUS_CMD_SIZE);
  if (ret != MODBUS_CMD_SIZE)
    {
      return -1;
    }

  tcdrain(fd);
  return 0;
}

/****************************************************************************
 * Name: soil_sensor_receive_response
 *
 * Description:
 *   接收 Modbus 响应
 *
 ****************************************************************************/

static int soil_sensor_receive_response(int fd, uint8_t *resp, size_t resp_size)
{
  int ret;
  int total = 0;
  fd_set rfds;
  struct timeval tv;

  while (total < MODBUS_RESP_SIZE)
    {
      FD_ZERO(&rfds);
      FD_SET(fd, &rfds);

      tv.tv_sec = 0;
      tv.tv_usec = 500000;  /* 500ms */

      ret = select(fd + 1, &rfds, NULL, NULL, &tv);

      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -1;
        }

      if (ret == 0)
        {
          /* 超时：已收到部分字节就返回部分数据（便于诊断异常帧/半截帧），
           * 一个字节都没收到才算"无应答"。 */

          return total > 0 ? total : -1;
        }

      if (FD_ISSET(fd, &rfds))
        {
          ret = read(fd, resp + total, resp_size - total);
          if (ret < 0)
            {
              return -1;
            }

          total += ret;
        }
    }

  return total;
}

/****************************************************************************
 * Name: soil_sensor_parse_data
 *
 * Description:
 *   解析传感器数据
 *
 ****************************************************************************/

static void soil_sensor_parse_data(const uint8_t *resp,
                                   struct soil_data_s *data)
{
  uint16_t raw_value;

  /* 寄存器0：温度（有符号16位） */

  raw_value = (resp[3] << 8) | resp[4];
  if (raw_value & 0x8000)
    {
      /* 负数：补码转换 */

      data->temp = -(float)(0x10000 - raw_value) / 10.0f;
    }
  else
    {
      data->temp = (float)raw_value / 10.0f;
    }

  /* 寄存器1：湿度（无符号16位） */

  raw_value = (resp[5] << 8) | resp[6];
  data->moisture = (float)raw_value / 10.0f;

  /* 寄存器2：电导率（无符号16位） */

  raw_value = (resp[7] << 8) | resp[8];
  data->ec = (float)raw_value;

  /* 寄存器3：盐分（无符号16位） */

  raw_value = (resp[9] << 8) | resp[10];
  data->salt = (float)raw_value;

  /* 寄存器4：氮（无符号16位） */

  raw_value = (resp[11] << 8) | resp[12];
  data->nitrogen = (float)raw_value;

  /* 寄存器5：磷（无符号16位） */

  raw_value = (resp[13] << 8) | resp[14];
  data->phosphorus = (float)raw_value;

  /* 寄存器6：钾（无符号16位） */

  raw_value = (resp[15] << 8) | resp[16];
  data->potassium = (float)raw_value;

  /* 寄存器7：pH（无符号16位） */

  raw_value = (resp[17] << 8) | resp[18];
  data->ph = (float)raw_value / 10.0f;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: soil_sensor_frame_valid
 *
 * Description:
 *   2026-09-11：Modbus RTU 整帧校验（地址 + 功能码 + 字节数 + CRC16）。
 *
 *   为什么必须校验：实测摄像头释放引脚后，土壤 UART 的 RX 悬空会读到
 *   "00 08 44 FF FF FF …" 这类自身 TX 串扰 + 空闲高电平噪声，长度恰好
 *   也是 21 字节。旧实现只看"长度够 21"就当有效 → 把 0xFFFF 解析成
 *   65535 上报服务器（湿度 6553.5% 这种假数据）。用户定案：宁可不报，
 *   绝不发假值。所以整帧校验不过一律丢弃。
 *
 *   Returned Value: 1 = 有效；0 = 无效
 ****************************************************************************/

int soil_sensor_frame_valid(const uint8_t *resp, int len, uint8_t addr)
{
  uint16_t crc;
  int i;

  if (resp == NULL || len < MODBUS_RESP_SIZE)
    {
      return 0;
    }

  /* 头三字节：从站地址 / 功能码 0x03 / 字节数 16（8 寄存器 ×2） */

  if (resp[0] != addr || resp[1] != 0x03 || resp[2] != 16)
    {
      return 0;
    }

  /* 全 0xFF（RX 悬空/总线上无设备）直接判无效 */

  for (i = 0; i < MODBUS_RESP_SIZE; i++)
    {
      if (resp[i] != 0xFF)
        {
          break;
        }
    }

  if (i == MODBUS_RESP_SIZE)
    {
      return 0;
    }

  /* CRC16（低字节在前），对前 19 字节求值 */

  crc = soil_sensor_crc16(resp, MODBUS_RESP_SIZE - 2);
  return (resp[MODBUS_RESP_SIZE - 2] == (uint8_t)(crc & 0xFF) &&
          resp[MODBUS_RESP_SIZE - 1] == (uint8_t)((crc >> 8) & 0xFF)) ? 1 : 0;
}

/****************************************************************************
 * Name: soil_sensor_probe
 *
 * Description:
 *   2026-09-11 排查用：按指定【从站地址 + 波特率】发一帧 Modbus 03 查询，
 *   返回实际收到的字节数（-1 = 一个字节都没收到）。
 *   plant soil scan 用它扫地址×波特率，判断"总线上到底有没有设备应答"——
 *   用来区分【接线/供电问题】和【地址/波特率不匹配】。
 *
 ****************************************************************************/

int soil_sensor_probe(int addr, int baudrate, uint8_t *resp, int resp_size)
{
  uint8_t cmd[8];
  uint16_t crc;
  int fd;
  int ret;
  int total;

  if (resp == NULL || resp_size <= 0)
    {
      return -1;
    }

  pthread_mutex_lock(&g_uart_lock);

  fd = open(g_dev, O_RDWR | O_NOCTTY);
  if (fd < 0)
    {
      printf("[SOIL] probe open %s failed: %d\n", g_dev, errno);
      pthread_mutex_unlock(&g_uart_lock);
      return -1;
    }

  if (soil_sensor_configure_uart(fd, baudrate) < 0)
    {
      close(fd);
      pthread_mutex_unlock(&g_uart_lock);
      return -1;
    }

  tcflush(fd, TCIFLUSH);

  cmd[0] = (uint8_t)addr;
  cmd[1] = 0x03;
  cmd[2] = 0x00;
  cmd[3] = 0x00;
  cmd[4] = 0x00;
  cmd[5] = 0x08;
  crc = soil_sensor_crc16(cmd, 6);
  cmd[6] = crc & 0xFF;
  cmd[7] = (crc >> 8) & 0xFF;

  total = 0;

  if (write(fd, cmd, 8) == 8)
    {
      tcdrain(fd);

      /* 2026-09-11 修复：必须"收满整帧"再判定。
       * 旧实现只 select 一次就 read 一次，read 只能拿到当时 FIFO 里的头
       * 几个字节（实测真设备应答只读到 1 B: 02），半截帧过不了 frame_valid
       * → 被归到 noise，scan 误报 hits=0。
       * 改为复用读取路径的收帧循环：收满 21 B，或 500ms 空闲超时才放弃。 */

      ret = soil_sensor_receive_response(fd, resp, (size_t)resp_size);
      if (ret > 0)
        {
          total = ret;
        }
    }

  close(fd);
  pthread_mutex_unlock(&g_uart_lock);

  return total > 0 ? total : -1;
}

int soil_sensor_init(const char *dev, int baudrate)
{
  /* 只记参数，不 open：谁读谁自己 open/close（见 g_uart_lock 上方说明） */

  if (dev)
    {
      strncpy(g_dev, dev, sizeof(g_dev) - 1);
      g_dev[sizeof(g_dev) - 1] = '\0';
    }

  if (baudrate > 0)
    {
      g_baudrate = baudrate;
    }

  /* 构建 Modbus 命令 */

  soil_sensor_build_command(SOIL_SENSOR_ADDR_DEFAULT, 0, 8);

  printf("[SOIL] Initializing: %s @ %d\n", g_dev, g_baudrate);
  return 0;
}

void soil_sensor_deinit(void)
{
  /* fd 每次事务自开自关，这里没有常驻资源要释放 */
}

int soil_sensor_read(struct soil_data_s *data, int verbose)
{
  uint8_t resp[MODBUS_RESP_SIZE + 4];  /* 额外空间 */
  int fd;
  int ret;
  int i;

  if (!data)
    {
      return -1;
    }

  pthread_mutex_lock(&g_uart_lock);

  if (g_paused)
    {
      /* 摄像头正占用 IO42/40（拍照页打开中）：拒绝收发，防止两条线对撞 */

      if (verbose)
        {
          printf("[SOIL] 摄像头占用中，土壤已挂起\n");
        }

      pthread_mutex_unlock(&g_uart_lock);
      return -1;
    }

  /* 本任务组自己打开串口（不缓存 fd，见文件头说明） */

  fd = open(g_dev, O_RDWR | O_NOCTTY);
  if (fd < 0)
    {
      if (verbose)
        {
          printf("[SOIL] open %s failed: %d\n", g_dev, errno);
        }

      pthread_mutex_unlock(&g_uart_lock);
      return -1;
    }

  if (soil_sensor_configure_uart(fd, g_baudrate) < 0)
    {
      close(fd);
      pthread_mutex_unlock(&g_uart_lock);
      return -1;
    }

  /* 丢弃上一轮残留脏字节 */

  tcflush(fd, TCIFLUSH);

  if (verbose)
    {
      printf("[SOIL] tx 8 B:");
      for (i = 0; i < 8; i++)
        {
          printf(" %02X", g_modbus_cmd[i]);
        }

      printf("\n");
    }

  /* 发送 Modbus 命令 */

  if (soil_sensor_send_command(fd) < 0)
    {
      if (verbose)
        {
          printf("[SOIL] write failed, errno=%d\n", errno);
        }

      close(fd);
      pthread_mutex_unlock(&g_uart_lock);
      return -1;
    }

  /* 接收响应 */

  ret = soil_sensor_receive_response(fd, resp, sizeof(resp));
  if (ret < 0)
    {
      if (verbose)
        {
          printf("[SOIL] recv: 超时无应答\n");
        }

      close(fd);
      pthread_mutex_unlock(&g_uart_lock);
      return -1;
    }

  if (verbose)
    {
      /* 调试：打印原始响应帧（定位异常帧/噪声用） */

      printf("[SOIL] rx %d B:", ret);
      for (i = 0; i < ret; i++)
        {
          printf(" %02X", resp[i]);
        }
      printf("\n");
    }

  if (ret < MODBUS_RESP_SIZE)
    {
      if (verbose)
        {
          printf("[SOIL] rx short (%d < %d) - parse skipped\n",
                 ret, MODBUS_RESP_SIZE);
        }

      close(fd);
      pthread_mutex_unlock(&g_uart_lock);
      return -1;
    }

  /* 整帧校验（地址/功能码/字节数/CRC）：不过就是噪声或没设备，
   * 直接丢弃 —— 绝不把 0xFFFF 之类的垃圾当真实值上报。 */

  if (!soil_sensor_frame_valid(resp, ret, SOIL_SENSOR_ADDR_DEFAULT))
    {
      if (verbose)
        {
          printf("[SOIL] 帧校验失败（噪声/无设备）— 丢弃，不上报\n");
        }

      close(fd);
      pthread_mutex_unlock(&g_uart_lock);
      return -1;
    }

  /* 解析数据 */

  soil_sensor_parse_data(resp, data);

  close(fd);
  pthread_mutex_unlock(&g_uart_lock);
  return 0;
}

void soil_sensor_set_paused(bool paused)
{
  pthread_mutex_lock(&g_uart_lock);
  g_paused = paused;
  pthread_mutex_unlock(&g_uart_lock);
}

void soil_sensor_print_data(const struct soil_data_s *data)
{
  if (!data)
    {
      return;
    }

  printf("[SOIL] ====== Soil Sensor Data ======\n");
  printf("[SOIL] Temp:     %.1f C\n", data->temp);
  printf("[SOIL] Moisture: %.1f %%\n", data->moisture);
  printf("[SOIL] EC:       %.0f uS/cm\n", data->ec);
  printf("[SOIL] Salt:     %.0f\n", data->salt);
  printf("[SOIL] N:        %.0f mg/kg\n", data->nitrogen);
  printf("[SOIL] P:        %.0f mg/kg\n", data->phosphorus);
  printf("[SOIL] K:        %.0f mg/kg\n", data->potassium);
  printf("[SOIL] pH:       %.1f\n", data->ph);
  printf("[SOIL] ==============================\n");
}
