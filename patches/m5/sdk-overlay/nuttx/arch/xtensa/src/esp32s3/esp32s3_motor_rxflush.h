/* UART1 adapter for the official SCSerial::rFlushSCS -> uart_flush_input.
 * IDF flush excludes RX, discards software bytes, resets RX FIFO, then
 * restores configured RX interrupts. It never resets TX, pins or clocks. */
#ifdef CONFIG_ESP32S3_UART1
/* Shared selector is defined later in esp32s3_serial.c. */
static bool g_uart1_rxpoll;
static uint32_t g_motor_rxflush_calls,g_motor_rxflush_sw,g_motor_rxflush_hw;
int esp32s3_uart1_rxflush(void)
{
 struct uart_dev_s *dev=&g_uart1_dev;
 irqstate_t upper=uart_spinlock(dev,true);
 irqstate_t lower=spin_lock_irqsave(&g_uart1_config.lock);
 int rc=0;uint32_t rxmask=UART_RXFIFO_TOUT_INT_ENA_M|UART_RXFIFO_FULL_INT_ENA_M;
 if(dev->open_count!=1){rc=dev->open_count?-EBUSY:-ENODEV;goto done;}
 if(dev->xmit.head!=dev->xmit.tail ||
    (getreg32(UART_STATUS_REG(1))&UART_TXFIFO_CNT_M) ||
    (getreg32(UART_FSM_STATUS_REG(1))&UART_ST_UTX_OUT_M))
   {rc=-EBUSY;goto done;}
 modifyreg32(UART_INT_ENA_REG(1),rxmask,0);
 unsigned size=dev->recv.size;
 if(size)g_motor_rxflush_sw+=(dev->recv.head+size-dev->recv.tail)%size;
 g_motor_rxflush_hw+=getreg32(UART_STATUS_REG(1))&0x3ff;
 dev->recv.tail=dev->recv.head;
 /* Exact ESP-IDF5.5.4 uart_ll_rxfifo_rst: pulse rxfifo_rst1 then0. */
 modifyreg32(UART_CONF0_REG(1),UART_RXFIFO_RST_M,UART_RXFIFO_RST_M);
 modifyreg32(UART_CONF0_REG(1),UART_RXFIFO_RST_M,0);
 putreg32(rxmask,UART_INT_CLR_REG(1));
 if(!g_uart1_rxpoll)
   {modifyreg32(UART_CONF1_REG(1),UART_RX_TOUT_EN_M,UART_RX_TOUT_EN_M);
    modifyreg32(UART_INT_ENA_REG(1),rxmask,rxmask);}
 else modifyreg32(UART_CONF1_REG(1),UART_RX_TOUT_EN_M,0);
 g_motor_rxflush_calls++;
 done:
 spin_unlock_irqrestore(&g_uart1_config.lock,lower);
 uart_spinunlock(dev,true,upper);return rc;
}
int esp32s3_uart1_rxflush_metrics(uint32_t v[3])
{if(!v)return -EINVAL;irqstate_t f=spin_lock_irqsave(&g_uart1_config.lock);
 v[0]=g_motor_rxflush_calls;v[1]=g_motor_rxflush_sw;v[2]=g_motor_rxflush_hw;
 spin_unlock_irqrestore(&g_uart1_config.lock,f);return 0;}
#endif
