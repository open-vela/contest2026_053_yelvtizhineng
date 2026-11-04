/* Match only UART1 RX fields observed in the unmodified official image.
 * Keep hardware corrupt-frame rejection, TX policy and IRQ masks unchanged.
 * Packet arrival/parse deadlines remain enforced by the existing parser. */
static void motor_uart1_rx_policy(unsigned id)
{
  if(id!=1)return;
  modifyreg32(UART_CONF1_REG(1),UART_RXFIFO_FULL_THRHD_M,
              120u<<UART_RXFIFO_FULL_THRHD_S);
  modifyreg32(UART_MEM_CONF_REG(1),UART_RX_TOUT_THRHD_M,
              100u<<UART_RX_TOUT_THRHD_S);
}
