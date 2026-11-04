/****************************************************************************
 * boards/xtensa/esp32s3/m5stack-cores3/src/esp32s3-cores3.h
 *
 * M5Stack CoreS3 board definitions (StackChan main controller).
 ****************************************************************************/

#ifndef __BOARDS_XTENSA_ESP32S3_M5STACK_CORES3_SRC_ESP32S3_CORES3_H
#define __BOARDS_XTENSA_ESP32S3_M5STACK_CORES3_SRC_ESP32S3_CORES3_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>
#include <stdint.h>

#include "esp32s3_spi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BUTTON_BOOT           0

/* Display: SPI3, ILI9342C/ILI9342E, 320x240, RGB565 */

#define DISPLAY_SPI           3
#define DISPLAY_CS            3
#define DISPLAY_DC            35
#define DISPLAY_SCLK          36
#define DISPLAY_MOSI          37
#define DISPLAY_BCKL_EXP_PIN  9

/* Internal I2C0 */

#define BOARD_I2C_BUS         0
#define BOARD_I2C_FREQ        (400 * 1000)
#define BOARD_I2C_SCL         11
#define BOARD_I2C_SDA         12

/* AW9523B I/O expander */

#define BOARD_IOEXP_ADDR      0x58
#define BOARD_IOEXP_LCD_RST   9       /* P1_1 */
#define BOARD_IOEXP_CAM_RST   8       /* P1_0 */
/* Official CoreS3 schematic U10, matching StackChan Aw9523 init 0x07/0x18. */
#define BOARD_IOEXP_TOUCH_RST 0      /* P0_0: FT6336 reset, active low */
#define BOARD_IOEXP_BUS_OUT_EN 1     /* P0_1: USB VBUS to system 5V switch */
#define BOARD_IOEXP_AW_RST    2      /* P0_2: AW88298 reset, active low */
#define BOARD_IOEXP_AUDIO_EN  BOARD_IOEXP_AW_RST
#define BOARD_IOEXP_ES_INT    3      /* P0_3: ES7210 interrupt input */
#define BOARD_IOEXP_TF_DETECT 4      /* P0_4: microSD card-detect input */
#define BOARD_IOEXP_USB_OTG_EN 5     /* P0_5: active-high OTG direction control */

/* AXP2101 PMIC */

#define BOARD_PMIC_ADDR       0x34
#define BOARD_PMIC_REG_EN     0x90
#define BOARD_PMIC_ALDO1      0x92
#define BOARD_PMIC_ALDO2      0x93
#define BOARD_PMIC_ALDO3      0x94
#define BOARD_PMIC_ALDO4      0x95
#define BOARD_PMIC_DLDO1      0x99

/* Touch: FT6336U, powered/reset through AW9523B. GPIO21 is AW9523B INTN. */

#define TOUCHSCEEN_I2C        BOARD_I2C_BUS
#define TOUCHSCEEN_ADDR       0x38
#define TOUCHSCEEN_CLOCK      BOARD_I2C_FREQ
#define TOUCHSCEEN_INT        21

/* Camera: GC0308 DVP 8-bit. The XCLK is generated on the camera board. */

#define BOARD_CAMERA_SCCB_ADDR 0x21
#define BOARD_CAMERA_PCLK      45
#define BOARD_CAMERA_VSYNC     46
#define BOARD_CAMERA_HREF      38
#define BOARD_CAMERA_D0        39
#define BOARD_CAMERA_D1        40
#define BOARD_CAMERA_D2        41
#define BOARD_CAMERA_D3        42
#define BOARD_CAMERA_D4        15
#define BOARD_CAMERA_D5        16
#define BOARD_CAMERA_D6        48
#define BOARD_CAMERA_D7        47

/* SD card shares SPI3 with the LCD and uses a second software CS. */

#define SD_SPI_CS             4

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

int esp32s3_bringup(void);

#ifdef CONFIG_ESP32S3_SPIFLASH
int board_spiflash_init(void);
#endif

#ifdef CONFIG_ESP32S3_CORES3_LCD
int board_lcd_initialize(void);
struct lcd_dev_s *board_lcd_getdev(int lcddev);
void board_lcd_uninitialize(void);
#endif

#ifdef CONFIG_ESP32S3_BOARD_TOUCHSCREEN
int board_touchscreen_initialize(void);
#endif

/* Board control helpers shared by LCD, touch, camera and audio. */

int board_cores3_ioexp_init(void);
int board_cores3_ioexp_set_dir(uint8_t pin, bool output);
int board_cores3_ioexp_write(uint8_t pin, bool value);
int board_cores3_ioexp_read(uint8_t pin, bool *value);
int board_cores3_ioexp_read_reg(uint8_t reg, uint8_t *value);
int board_cores3_pmic_set_ldo(uint8_t reg, int millivolts);
int board_cores3_pmic_read_reg(uint8_t reg, uint8_t *value);
int board_cores3_backlight_set(int percent);
int board_cores3_sw_lcd_test(uint16_t color);
int board_cores3_touch_firmware_id(uint8_t *firmware_id);
int board_cores3_camera_power(bool enable);
int board_cores3_audio_power(bool enable);

int board_camera_sccb_probe(uint16_t *pid);
int board_camera_sccb_write(uint16_t reg, uint8_t val);
int board_camera_sccb_read(uint16_t reg, uint8_t *val);

/* Compatibility symbol used by the existing plant diagnostics UI. */
int gt911_diag_snapshot(uint32_t out[8]);

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_XTENSA_ESP32S3_M5STACK_CORES3_SRC_ESP32S3_CORES3_H */
