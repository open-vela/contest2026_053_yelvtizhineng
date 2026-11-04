/* Official uart_set_pin configures the electrical state at begin only.
 * Normal TX/RX phases never rewrite GPIO matrix, pad, output-enable or DATA.
 * Keep singleton ownership and empty TX queue/FIFO/FSM checks. A parked or
 * unexpected configuration still fails closed. TX input sampling is optional
 * diagnostic evidence: value2 means input buffer OFF, not a LOW/HIGH sample.
 */
#ifdef CONFIG_ESP32S3_UART1
extern int esp32s3_uart1_pins_parked(void);
static uint32_t g_halfduplex_drive,g_halfduplex_release,g_halfduplex_errors;
static uint32_t g_halfduplex_last[4];
#define MOTOR_HD_ROUTE (GPIO_FUNC0_OUT_SEL_CFG_REG+4*CONFIG_ESP32S3_UART1_TXPIN)
#define MOTOR_HD_PAD (REG_IO_MUX_BASE+4*(CONFIG_ESP32S3_UART1_TXPIN+1))
int esp32s3_uart1_halfduplex_tx(bool drive)
{
 struct uart_dev_s *dev=&g_uart1_dev;
 irqstate_t upper=uart_spinlock(dev,true);
 irqstate_t lower=spin_lock_irqsave(&g_uart1_config.lock);
 int rc=0;uint32_t pin=1u<<6;
 if(CONFIG_ESP32S3_UART1_TXPIN!=6 || CONFIG_ESP32S3_UART1_RXPIN!=7 || esp32s3_uart1_pins_parked())
   {rc=-ENODEV;goto done;}
 if(dev->open_count!=1){rc=dev->open_count?-EBUSY:-ENODEV;goto done;}
 if(dev->xmit.head!=dev->xmit.tail ||
    (getreg32(UART_STATUS_REG(1))&UART_TXFIFO_CNT_M) ||
    (getreg32(UART_FSM_STATUS_REG(1))&UART_ST_UTX_OUT_M))
   {rc=-EBUSY;goto done;}
 uint32_t route=getreg32(MOTOR_HD_ROUTE),pad=getreg32(MOTOR_HD_PAD);
 uint32_t rxpad=getreg32(REG_IO_MUX_BASE+4*(CONFIG_ESP32S3_UART1_RXPIN+1));
 uint32_t enabled=getreg32(GPIO_ENABLE_REG);
 if((route&GPIO_FUNC0_OUT_SEL_M)!=g_uart1_config.txsig ||
    (route&(GPIO_FUNC0_OUT_INV_SEL_M|GPIO_FUNC0_OEN_INV_SEL_M|GPIO_FUNC0_OEN_SEL_M)) ||
    ((pad&MCU_SEL_M)>>MCU_SEL_S)!=1 ||
    ((rxpad&MCU_SEL_M)>>MCU_SEL_S)!=0 || !(rxpad&FUN_IE) ||
    !(enabled&pin) || (enabled&(1u<<7)))
   {rc=-EPROTO;goto done;}
 unsigned level=(pad&FUN_IE)?((getreg32(GPIO_IN_REG)&pin)?1u:0u):2u;
 g_halfduplex_last[0]=drive;g_halfduplex_last[1]=(enabled&pin)!=0;
 g_halfduplex_last[2]=level;g_halfduplex_last[3]=route;
 if(level==0){rc=-EIO;goto done;}
 if(drive)g_halfduplex_drive++;else g_halfduplex_release++;
 done:
 if(rc<0)g_halfduplex_errors++;
 spin_unlock_irqrestore(&g_uart1_config.lock,lower);
 uart_spinunlock(dev,true,upper);return rc;
}
int esp32s3_uart1_halfduplex_metrics(uint32_t *v,unsigned n)
{
 if(!v || n<8)return -EINVAL;
 irqstate_t lower=spin_lock_irqsave(&g_uart1_config.lock);
 v[0]=g_halfduplex_drive;v[1]=g_halfduplex_release;v[2]=g_halfduplex_errors;
 for(unsigned i=0;i<4;i++)v[i+3]=g_halfduplex_last[i];
 v[7]=esp32s3_uart1_pins_parked();
 spin_unlock_irqrestore(&g_uart1_config.lock,lower);return 0;
}
/* One body-worker owner; timestamps only, no hardware changes. */
static uint32_t g_motor_goal_stamp[2];
static bool g_motor_goal_stamp_valid[2];
void esp32s3_uart1_goal_stamp(unsigned id,uint32_t at)
{if(id>=1 && id<=2){g_motor_goal_stamp[id-1]=at;g_motor_goal_stamp_valid[id-1]=true;}}
bool esp32s3_uart1_goal_stamp_get(unsigned id,uint32_t *at)
{if(id<1 || id>2 || !at || !g_motor_goal_stamp_valid[id-1])return false;
 *at=g_motor_goal_stamp[id-1];return true;}
#endif
