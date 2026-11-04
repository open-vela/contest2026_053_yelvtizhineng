/* Default-OFF, receive-only decoder of the actual GPIO7 UART1 RX pad.
 * Uses otherwise unopened UART2. Never writes a FIFO or the UART1 registers.
 * Bounded motor protocol bytes only; no user speech/media/RAM capture.
 */
#include <stdatomic.h>
#include <nuttx/mutex.h>
#include "hardware/esp32s3_gpio_sigmap.h"

static _Atomic bool g_motor_txmirror_on;
static bool g_motor_txproof_pad;
/* Numeric first mismatching TX byte only; retained for post-cut snapshot. */
static volatile int g_txproof_mismatch_detail[4];
static bool g_motor_txmirror_active;
static unsigned g_rxmirror_ontime;
static uint8_t g_rxmirror_last[64];
static unsigned g_rxmirror_last_count;
static uint32_t g_motor_txmirror_saved[8];
static spinlock_t g_motor_txmirror_lock=SP_UNLOCKED;
static const uint32_t g_motor_txmirror_regs[7]={
 UART_CONF0_REG(2),UART_CONF1_REG(2),UART_CLKDIV_REG(2),
 UART_CLK_CONF_REG(2),UART_MEM_CONF_REG(2),
 UART_RX_FILT_REG(2),GPIO_FUNC0_IN_SEL_CFG_REG+4*U2RXD_IN_IDX};
#define MOTOR_TXMIRROR_GPIO (g_motor_txproof_pad?CONFIG_ESP32S3_UART1_TXPIN:CONFIG_ESP32S3_UART1_RXPIN)
#define MOTOR_TXMIRROR_PAD (REG_IO_MUX_BASE+4*(MOTOR_TXMIRROR_GPIO+1))
#define MOTOR_TXMIRROR_RX_ERRORS 0x89cu

static bool motor_txmirror_reserved(void){return g_motor_txmirror_on;}
static void motor_txmirror_reset_fifo(void)
{
 modifyreg32(UART_CONF0_REG(2),0,UART_RXFIFO_RST_M);
 modifyreg32(UART_CONF0_REG(2),UART_RXFIFO_RST_M,0);
}

int esp32s3_uart1_txmirror_config(bool on)
{
 int rc=nxmutex_trylock(&g_uart2_dev.closelock);if(rc<0)return rc;
 irqstate_t flags=spin_lock_irqsave(&g_motor_txmirror_lock);
 if(on==g_motor_txmirror_on){rc=0;goto done;}
 if(on)
  {
   if(g_uart2_dev.open_count || g_uart2_dev.xmit.head!=g_uart2_dev.xmit.tail ||
      g_uart2_dev.recv.head!=g_uart2_dev.recv.tail ||
      getreg32(UART_INT_ENA_REG(2)) ||
      (getreg32(UART_STATUS_REG(2))&(UART_RXFIFO_CNT_M|UART_TXFIFO_CNT_M)) ||
      (getreg32(UART_INT_RAW_REG(2))&MOTOR_TXMIRROR_RX_ERRORS) ||
      esp32s3_uart1_wire_counter_enabled())
     {rc=-EBUSY;goto done;}
   for(unsigned i=0;i<7;i++)g_motor_txmirror_saved[i]=getreg32(g_motor_txmirror_regs[i]);
   g_motor_txmirror_saved[7]=getreg32(MOTOR_TXMIRROR_PAD)&FUN_IE;
   /* Reservation is visible to generic UART2 setup before config changes. */
   g_motor_txmirror_on=true;
   /* Independent fixed40MHz XTAL reference (source3 / divider40).
    * Sharing UART1's APB clock could hide a transmitter baud-clock fault. */
   putreg32((getreg32(UART_CLK_CONF_REG(1))&~(UART_TX_SCLK_EN_M|UART_SCLK_SEL_M))|UART_RX_SCLK_EN_M|UART_SCLK_EN_M|(3u<<UART_SCLK_SEL_S),UART_CLK_CONF_REG(2));
   putreg32(40,UART_CLKDIV_REG(2));
   putreg32(getreg32(UART_CONF0_REG(1)),UART_CONF0_REG(2));
   putreg32(0,UART_CONF1_REG(2));
   putreg32((1u<<UART_RX_SIZE_S)|(1u<<UART_TX_SIZE_S),UART_MEM_CONF_REG(2));
   putreg32(getreg32(UART_RX_FILT_REG(1)),UART_RX_FILT_REG(2));
   modifyreg32(MOTOR_TXMIRROR_PAD,0,FUN_IE);
   putreg32(0x80u|MOTOR_TXMIRROR_GPIO,GPIO_FUNC0_IN_SEL_CFG_REG+4*U2RXD_IN_IDX);
   motor_txmirror_reset_fifo();g_motor_txmirror_active=false;g_rxmirror_ontime=g_rxmirror_last_count=0;memset(g_rxmirror_last,0,sizeof(g_rxmirror_last));rc=0;
  }
 else
  {
   if(g_motor_txmirror_active || esp32s3_uart1_wire_counter_enabled())
     {rc=-EBUSY;goto done;}
   motor_txmirror_reset_fifo();
   putreg32(MOTOR_TXMIRROR_RX_ERRORS,UART_INT_CLR_REG(2));
   for(unsigned i=0;i<7;i++)putreg32(g_motor_txmirror_saved[i],g_motor_txmirror_regs[i]);
   modifyreg32(MOTOR_TXMIRROR_PAD,FUN_IE,g_motor_txmirror_saved[7]);
   rc=0;for(unsigned i=0;i<7;i++)if(getreg32(g_motor_txmirror_regs[i])!=g_motor_txmirror_saved[i])rc=-EIO;
   if((getreg32(MOTOR_TXMIRROR_PAD)&FUN_IE)!=g_motor_txmirror_saved[7])rc=-EIO;
   if(rc==0){g_motor_txmirror_on=false;g_rxmirror_ontime=g_rxmirror_last_count=0;memset(g_rxmirror_last,0,sizeof(g_rxmirror_last));}
  }
done:
 spin_unlock_irqrestore(&g_motor_txmirror_lock,flags);
 nxmutex_unlock(&g_uart2_dev.closelock);return rc;
}

/* RX-only observation of physical TX6 using a separate XTAL-clock UART.
 * No UART1 register, TX route, output data or deadline changes. */
int esp32s3_uart1_txproof_config(bool on)
{
 if(on){if(g_motor_txmirror_on)return -EBUSY;g_motor_txproof_pad=true;
  int rc=esp32s3_uart1_txmirror_config(true);if(rc<0)g_motor_txproof_pad=false;return rc;}
 if(!g_motor_txproof_pad)return 0;
 int rc=esp32s3_uart1_txmirror_config(false);if(rc==0)g_motor_txproof_pad=false;return rc;
}

/* Normal UART begin/resume owns pin mux initialization. Re-enable ONLY
 * the optional TX input sample after that initialization and before traffic. */
int esp32s3_uart1_txproof_rearm_input(void)
{
 irqstate_t flags=spin_lock_irqsave(&g_motor_txmirror_lock);int rc=-ENOTSUP;
 if(g_motor_txmirror_on && g_motor_txproof_pad){
  modifyreg32(MOTOR_TXMIRROR_PAD,0,FUN_IE);
  rc=(getreg32(MOTOR_TXMIRROR_PAD)&FUN_IE)?0:-EIO;
 }
 spin_unlock_irqrestore(&g_motor_txmirror_lock,flags);return rc;
}

int esp32s3_uart1_txmirror_begin(void)
{
 irqstate_t flags=spin_lock_irqsave(&g_motor_txmirror_lock);int rc=0;
 if(!g_motor_txmirror_on)rc=-ENOTSUP;
 else if(g_motor_txmirror_active)rc=-EBUSY;
 else {motor_txmirror_reset_fifo();putreg32(MOTOR_TXMIRROR_RX_ERRORS,UART_INT_CLR_REG(2));g_motor_txmirror_active=true;g_rxmirror_ontime=0;g_rxmirror_last_count=0;memset(g_rxmirror_last,0,sizeof(g_rxmirror_last));}
 spin_unlock_irqrestore(&g_motor_txmirror_lock,flags);return rc;
}

/* v={observed byte count, exact match, fresh error bits, first mismatch}.
 * Decode stays local: only numeric comparison results leave this helper. */
int esp32s3_uart1_txmirror_end(const uint8_t *expected,unsigned length,int v[4])
{
 if(!expected || !v || !length || length>24)return -EINVAL;
 irqstate_t flags=spin_lock_irqsave(&g_motor_txmirror_lock);int rc=0;
 if(!g_motor_txmirror_on || !g_motor_txmirror_active || !g_motor_txproof_pad){rc=-ENOTSUP;goto done;}
 v[0]=getreg32(UART_STATUS_REG(2))&UART_RXFIFO_CNT_M;v[1]=0;
 v[2]=getreg32(UART_INT_RAW_REG(2))&MOTOR_TXMIRROR_RX_ERRORS;v[3]=-1;
 g_rxmirror_last_count=v[0]>64?64:v[0];
 for(unsigned i=0;i<g_rxmirror_last_count;i++){
  uint8_t value=getreg32(UART_FIFO_REG(2));g_rxmirror_last[i]=value;
  if(value!=expected[i%length] && v[3]<0){v[3]=i;
   if(!g_txproof_mismatch_detail[0]){g_txproof_mismatch_detail[1]=i;g_txproof_mismatch_detail[2]=value;g_txproof_mismatch_detail[3]=expected[i%length];g_txproof_mismatch_detail[0]=1;}}
 }
 /* READ repeats remain inside the original window. Every byte must be
  * part of at most3 exact complete copies, not merely a matching prefix. */
 if(v[0]>64){rc=-EOVERFLOW;motor_txmirror_reset_fifo();}
 else v[1]=v[0]>0 && (unsigned)v[0]%length==0 &&
  (unsigned)v[0]<=3*length && v[3]<0 && !v[2];
 g_motor_txmirror_active=false;
 done:spin_unlock_irqrestore(&g_motor_txmirror_lock,flags);return rc;
}

/* Called only inside the ORIGINAL arrival check. One read-only FIFO count;
 * no payload drain, UART1 write or timing-window change. Default OFF. */
void esp32s3_uart1_rxmirror_observe(void)
{
 if(!g_motor_txmirror_on)return;
 irqstate_t flags=spin_lock_irqsave(&g_motor_txmirror_lock);
 if(g_motor_txmirror_active)
   {unsigned n=getreg32(UART_STATUS_REG(2))&UART_RXFIFO_CNT_M;
    if(n>g_rxmirror_ontime)g_rxmirror_ontime=n;}
 spin_unlock_irqrestore(&g_motor_txmirror_lock,flags);
}
/* Numeric only: total bytes, valid expectedID/length/status/CRC packet,
 * fresh hardware errors, bytes seen by the original arrival cutoff.
 * Caller cuts motor rail FIRST on any native fault, then reads this FIFO.
 * The64-byte protocol-only raw copy is RAM-only and erased on OFF. */
int esp32s3_uart1_rxmirror_end(unsigned id,unsigned payload,int v[4])
{
 if(!v || id<1 || id>2 || payload>58)return -EINVAL;
 irqstate_t flags=spin_lock_irqsave(&g_motor_txmirror_lock);int rc=0;
 if(!g_motor_txmirror_on || !g_motor_txmirror_active){rc=-ENOTSUP;goto done;}
 v[0]=getreg32(UART_STATUS_REG(2))&UART_RXFIFO_CNT_M;v[1]=0;
 v[2]=getreg32(UART_INT_RAW_REG(2))&MOTOR_TXMIRROR_RX_ERRORS;
 v[3]=g_rxmirror_ontime;g_rxmirror_last_count=v[0]>64?64:v[0];
 for(unsigned i=0;i<g_rxmirror_last_count;i++)g_rxmirror_last[i]=getreg32(UART_FIFO_REG(2));
 if(v[0]>64){rc=-EOVERFLOW;motor_txmirror_reset_fifo();}
 else if(v[0]==(int)payload+6 && !v[2])
   {unsigned sum=0;for(unsigned i=2;i<g_rxmirror_last_count;i++)sum+=g_rxmirror_last[i];
    v[1]=g_rxmirror_last[0]==255 && g_rxmirror_last[1]==255 &&
      g_rxmirror_last[2]==id && g_rxmirror_last[3]==payload+2 &&
      g_rxmirror_last[4]==0 && (sum&255)==255;}
 g_motor_txmirror_active=false;
 done:spin_unlock_irqrestore(&g_motor_txmirror_lock,flags);return rc;
}
