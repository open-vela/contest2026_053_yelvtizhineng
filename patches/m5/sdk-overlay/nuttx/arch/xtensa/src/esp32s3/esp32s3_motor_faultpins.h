/* Read-only fault witness BEFORE the mandatory motor rail cut. No locks,
 * waiting, interrupt masking, allocation, I2C, UART/FIFO/pin writes or logs.
 * Sequential samples are diagnostic evidence, not an atomic hardware state.
 */
#include "hardware/esp32s3_system.h"
int esp32s3_uart1_faultpins_capture(uint32_t *v,unsigned count)
{
 if(!v || count!=18)return -EINVAL;
 const uint32_t address[18]={GPIO_ENABLE_REG,GPIO_IN_REG,GPIO_OUT_REG,
 GPIO_FUNC0_OUT_SEL_CFG_REG+4*CONFIG_ESP32S3_UART1_TXPIN,
 GPIO_FUNC0_IN_SEL_CFG_REG+4*g_uart1_config.rxsig,
 REG_IO_MUX_BASE+4*(CONFIG_ESP32S3_UART1_TXPIN+1),
 REG_IO_MUX_BASE+4*(CONFIG_ESP32S3_UART1_RXPIN+1),
 UART_CONF0_REG(1),UART_CONF1_REG(1),UART_CLK_CONF_REG(1),UART_CLKDIV_REG(1),
 UART_STATUS_REG(1),UART_FSM_STATUS_REG(1),UART_INT_RAW_REG(1),
 GPIO_PIN0_REG+4*CONFIG_ESP32S3_UART1_TXPIN,
 GPIO_PIN0_REG+4*CONFIG_ESP32S3_UART1_RXPIN,
 SYSTEM_PERIP_CLK_EN0_REG,SYSTEM_PERIP_RST_EN0_REG};
 for(unsigned i=0;i<18;i++)v[i]=getreg32(address[i]);
 return 0;
}
