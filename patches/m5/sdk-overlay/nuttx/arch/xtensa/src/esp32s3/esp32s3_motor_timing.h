/* Default-OFF RMT receive-only observation of GPIO6. No UART/FIFO writes.
 * CONFIG_RMT users cannot coexist. Four bounded motor frames, no heap/ISR.
 * XTAL40MHz /20 =2MHz. Capture ends at100us idle; early end is evidence
 * of a long gap, never proof that the remainder of a frame was absent. */
#include "hardware/esp32s3_system.h"
#include "soc/rmt_reg.h"
#include "hal/rmt_ll.h"

static bool g_motor_timing_on,g_motor_timing_active,g_motor_scope_on;
static uint32_t g_motor_timing_saved[7],g_motor_timing_saved_ram[192];
struct motor_timing_frame {uint32_t number,id,length,rc,raw,count,complete;uint32_t wave[192];};
static struct motor_timing_frame g_motor_timing_frames[4];
static unsigned g_motor_timing_count;
#define MOTOR_TIMING_RAM ((volatile uint32_t *)0x60016b00u)
#define MOTOR_TIMING_ROUTE (GPIO_FUNC0_IN_SEL_CFG_REG+4*81)
#define MOTOR_TIMING_PAD (REG_IO_MUX_BASE+4*(CONFIG_ESP32S3_UART1_TXPIN+1))

int esp32s3_uart1_timing_enabled(void){return g_motor_timing_on;}
int esp32s3_uart1_timing_config(bool on)
{
#ifdef CONFIG_RMT
 return -EBUSY;
#else
 if(g_motor_scope_on || g_motor_timing_active)return -EBUSY;
 if(on==g_motor_timing_on)return 0;
 if(on)
  {
   if(getreg32(SYSTEM_PERIP_CLK_EN0_REG)&SYSTEM_RMT_CLK_EN)return -EBUSY;
   g_motor_timing_saved[0]=getreg32(SYSTEM_PERIP_CLK_EN0_REG)&SYSTEM_RMT_CLK_EN;
   g_motor_timing_saved[1]=getreg32(SYSTEM_PERIP_RST_EN0_REG)&SYSTEM_RMT_RST;
   g_motor_timing_saved[2]=getreg32(MOTOR_TIMING_ROUTE);
   g_motor_timing_saved[3]=getreg32(MOTOR_TIMING_PAD)&FUN_IE;
   modifyreg32(SYSTEM_PERIP_CLK_EN0_REG,0,SYSTEM_RMT_CLK_EN);
   modifyreg32(SYSTEM_PERIP_RST_EN0_REG,SYSTEM_RMT_RST,0);
   /* Refuse a peripheral with pending ownership rather than resetting it. */
   if(getreg32(RMT_INT_ENA_REG))goto busy;
   for(unsigned i=0;i<4;i++)if(RMT.chmconf[i].conf1.rx_en_chm || RMT.chnconf0[i].tx_start_chn)goto busy;
   g_motor_timing_saved[4]=getreg32(RMT_CH4CONF0_REG);
   g_motor_timing_saved[5]=getreg32(RMT_CH4CONF1_REG);
   g_motor_timing_saved[6]=getreg32(RMT_SYS_CONF_REG);
   rmt_ll_enable_periph_clock(&RMT,true);
   rmt_ll_enable_mem_access_nonfifo(&RMT,true);
   rmt_ll_mem_force_power_on(&RMT);
   for(unsigned i=0;i<192;i++)g_motor_timing_saved_ram[i]=MOTOR_TIMING_RAM[i];
   rmt_ll_set_group_clock_src(&RMT,0,RMT_CLK_SRC_XTAL,1,0,0);
   rmt_ll_enable_group_clock(&RMT,true);
   rmt_ll_rx_set_channel_clock_div(&RMT,0,20);
   rmt_ll_rx_set_mem_blocks(&RMT,0,4);
   rmt_ll_rx_set_idle_thres(&RMT,0,200);
   rmt_ll_rx_enable_filter(&RMT,0,false);
   rmt_ll_rx_enable_wrap(&RMT,0,false);
   rmt_ll_rx_enable_carrier_demodulation(&RMT,0,false);
   modifyreg32(MOTOR_TIMING_PAD,0,FUN_IE);
   putreg32(0x80u|CONFIG_ESP32S3_UART1_TXPIN,MOTOR_TIMING_ROUTE);
   memset(g_motor_timing_frames,0,sizeof(g_motor_timing_frames));g_motor_timing_count=0;
   g_motor_timing_on=true;return 0;
busy:
   modifyreg32(SYSTEM_PERIP_RST_EN0_REG,SYSTEM_RMT_RST,g_motor_timing_saved[1]);
   modifyreg32(SYSTEM_PERIP_CLK_EN0_REG,SYSTEM_RMT_CLK_EN,g_motor_timing_saved[0]);return -EBUSY;
  }
 rmt_ll_rx_enable(&RMT,0,false);
 rmt_ll_rx_set_mem_owner(&RMT,0,RMT_LL_MEM_OWNER_SW);
 for(unsigned i=0;i<192;i++)MOTOR_TIMING_RAM[i]=g_motor_timing_saved_ram[i];
 putreg32(g_motor_timing_saved[4],RMT_CH4CONF0_REG);
 putreg32(g_motor_timing_saved[5],RMT_CH4CONF1_REG);
 putreg32(g_motor_timing_saved[6],RMT_SYS_CONF_REG);
 putreg32(g_motor_timing_saved[2],MOTOR_TIMING_ROUTE);
 modifyreg32(MOTOR_TIMING_PAD,FUN_IE,g_motor_timing_saved[3]);
 bool exact=getreg32(MOTOR_TIMING_ROUTE)==g_motor_timing_saved[2] &&
  (getreg32(MOTOR_TIMING_PAD)&FUN_IE)==g_motor_timing_saved[3] &&
  getreg32(RMT_CH4CONF0_REG)==g_motor_timing_saved[4] && getreg32(RMT_SYS_CONF_REG)==g_motor_timing_saved[6];
 modifyreg32(SYSTEM_PERIP_RST_EN0_REG,SYSTEM_RMT_RST,g_motor_timing_saved[1]);
 modifyreg32(SYSTEM_PERIP_CLK_EN0_REG,SYSTEM_RMT_CLK_EN,g_motor_timing_saved[0]);
 g_motor_timing_on=false;return exact?0:-EIO;
#endif
}

int esp32s3_uart1_timing_begin(unsigned number,unsigned id,unsigned length)
{
 if(!g_motor_timing_on || g_motor_scope_on)return -ENOTSUP;
 if(g_motor_timing_active || id<1 || id>2 || length!=13)return -EINVAL;
 rmt_ll_rx_enable(&RMT,0,false);rmt_ll_rx_set_mem_owner(&RMT,0,RMT_LL_MEM_OWNER_SW);
 /* Hardware write cursor bounds every read; no pre-TX RAM sweep. */
 rmt_ll_rx_reset_pointer(&RMT,0);rmt_ll_rx_set_mem_owner(&RMT,0,RMT_LL_MEM_OWNER_HW);
 putreg32(RMT_CH4_RX_END_INT_RAW|RMT_CH4_ERR_INT_RAW,RMT_INT_CLR_REG);
 struct motor_timing_frame *f=&g_motor_timing_frames[g_motor_timing_count++%4];
 f->number=number;f->id=id;f->length=length;
 f->rc=f->raw=f->count=f->complete=0; /* Old wave tail is never read. */
 g_motor_timing_active=true;rmt_ll_rx_enable(&RMT,0,true);return 0;
}

int esp32s3_uart1_timing_end(int rc)
{
 if(!g_motor_timing_active)return -ENOTSUP;
 /* Only after the caller's immediate fault rail-cut. No deadline changes.
  * Successful ACK often already spans the100us idle threshold. */
 uint64_t until=esp32s3_rt_timer_time_us()+150;
 for(unsigned i=0;i<200;i++)
  {if(getreg32(RMT_INT_RAW_REG)&(RMT_CH4_RX_END_INT_RAW|RMT_CH4_ERR_INT_RAW))break;
   if(esp32s3_rt_timer_time_us()>=until)break;up_udelay(1);}
 struct motor_timing_frame *f=&g_motor_timing_frames[(g_motor_timing_count-1)%4];
 f->raw=getreg32(RMT_INT_RAW_REG);f->complete=(f->raw&RMT_CH4_RX_END_INT_RAW)!=0;f->rc=rc;
 rmt_ll_rx_enable(&RMT,0,false);rmt_ll_rx_set_mem_owner(&RMT,0,RMT_LL_MEM_OWNER_SW);
 /* Match the NuttX RMT RX-end handler: stop first, then read the
  * hardware writer cursor. Stale RAM outside that cursor is not evidence. */
 uint32_t used=rmt_ll_rx_get_memory_writer_offset(&RMT,0);
 if(used>192){f->raw|=RMT_CH4_ERR_INT_RAW;used=0;}
 f->count=used;
 for(unsigned i=0;i<used;i++)f->wave[i]=MOTOR_TIMING_RAM[i];
 g_motor_timing_active=false;return 0;
}

/* Default-OFF physical TX/RX scope. Two RMT RX channels,96 words each,
 * 2MHz(0.5us). Trigger ONLY immediately before the final READ retransmit.
 * No UART/FIFO writes, ISR, heap or per-normal-packet arming overhead.
 * Each channel ends after16.3835ms idle, NOT proof about the whole60ms.
 * Software must match meta.trace_number to the failed bus transaction;
 * pointer bounds and END/ERR bits determine whether a waveform is usable. */
static uint32_t g_motor_scope_saved[4];
static volatile uint32_t g_motor_scope_meta[12];
#define MOTOR_SCOPE_RX_ROUTE (GPIO_FUNC0_IN_SEL_CFG_REG+4*83)
#define MOTOR_SCOPE_RX_PAD (REG_IO_MUX_BASE+4*(CONFIG_ESP32S3_UART1_RXPIN+1))
int esp32s3_uart1_scope_config(bool on)
{
 if(on==g_motor_scope_on)return 0;
 if(on)
  {
   if(g_motor_timing_on || g_motor_timing_active)return -EBUSY;
   int rc=esp32s3_uart1_timing_config(true);if(rc<0)return rc;
   g_motor_scope_saved[0]=getreg32(RMT_CH6CONF0_REG);
   g_motor_scope_saved[1]=getreg32(RMT_CH6CONF1_REG);
   g_motor_scope_saved[2]=getreg32(MOTOR_SCOPE_RX_ROUTE);
   g_motor_scope_saved[3]=getreg32(MOTOR_SCOPE_RX_PAD)&FUN_IE;
   for(unsigned channel=0;channel<=2;channel+=2)
    {
     rmt_ll_rx_set_channel_clock_div(&RMT,channel,20);
     rmt_ll_rx_set_mem_blocks(&RMT,channel,2);
     rmt_ll_rx_set_idle_thres(&RMT,channel,32767);
     rmt_ll_rx_enable_filter(&RMT,channel,false);
     rmt_ll_rx_enable_wrap(&RMT,channel,false);
     rmt_ll_rx_enable_carrier_demodulation(&RMT,channel,false);
    }
   modifyreg32(MOTOR_SCOPE_RX_PAD,0,FUN_IE);
   putreg32(0x80u|CONFIG_ESP32S3_UART1_RXPIN,MOTOR_SCOPE_RX_ROUTE);
   memset((void *)g_motor_scope_meta,0,sizeof(g_motor_scope_meta));
   g_motor_scope_on=true;g_motor_scope_meta[0]=1;return 0;
  }
 for(unsigned channel=0;channel<=2;channel+=2)
  {rmt_ll_rx_enable(&RMT,channel,false);rmt_ll_rx_set_mem_owner(&RMT,channel,RMT_LL_MEM_OWNER_SW);}
 putreg32(g_motor_scope_saved[0],RMT_CH6CONF0_REG);
 putreg32(g_motor_scope_saved[1],RMT_CH6CONF1_REG);
 putreg32(g_motor_scope_saved[2],MOTOR_SCOPE_RX_ROUTE);
 modifyreg32(MOTOR_SCOPE_RX_PAD,FUN_IE,g_motor_scope_saved[3]);
 g_motor_scope_on=false;g_motor_scope_meta[0]=0;
 return esp32s3_uart1_timing_config(false);
}
int esp32s3_uart1_scope_trigger(unsigned number,unsigned id,unsigned reg,unsigned length)
{
 if(!g_motor_scope_on)return -ENOTSUP;
 if((id<1 || (id>2 && id!=0xfe)) || length<1 || length>29)return -EINVAL;
 for(unsigned channel=0;channel<=2;channel+=2)
  {rmt_ll_rx_enable(&RMT,channel,false);rmt_ll_rx_set_mem_owner(&RMT,channel,RMT_LL_MEM_OWNER_SW);
   rmt_ll_rx_reset_pointer(&RMT,channel);rmt_ll_rx_set_mem_owner(&RMT,channel,RMT_LL_MEM_OWNER_HW);}
 putreg32(RMT_CH4_RX_END_INT_RAW|RMT_CH4_ERR_INT_RAW|RMT_CH6_RX_END_INT_RAW|RMT_CH6_ERR_INT_RAW,RMT_INT_CLR_REG);
 g_motor_scope_meta[1]++;g_motor_scope_meta[2]=number;g_motor_scope_meta[3]=id;
 g_motor_scope_meta[4]=reg;g_motor_scope_meta[5]=length;
 g_motor_scope_meta[6]=(uint32_t)esp32s3_rt_timer_time_us();
 g_motor_scope_meta[7]=0;g_motor_scope_meta[8]=g_motor_scope_meta[9]=0;
 g_motor_scope_meta[10]=g_motor_scope_meta[11]=0;
 rmt_ll_rx_enable(&RMT,0,true);rmt_ll_rx_enable(&RMT,2,true);return 0;
}
int esp32s3_uart1_scope_freeze_after_cut(void)
{
 if(!g_motor_scope_on)return -ENOTSUP;
 for(unsigned channel=0;channel<=2;channel+=2)
  {rmt_ll_rx_enable(&RMT,channel,false);rmt_ll_rx_set_mem_owner(&RMT,channel,RMT_LL_MEM_OWNER_SW);}
 g_motor_scope_meta[7]=getreg32(RMT_INT_RAW_REG);
 g_motor_scope_meta[8]=rmt_ll_rx_get_memory_writer_offset(&RMT,0);
 g_motor_scope_meta[9]=rmt_ll_rx_get_memory_writer_offset(&RMT,2);
 g_motor_scope_meta[10]=(uint32_t)esp32s3_rt_timer_time_us();
 g_motor_scope_meta[11]=1;return 0;
}
int esp32s3_uart1_scope_status(uint32_t *v,unsigned n)
{if(!v || n<12)return -EINVAL;for(unsigned i=0;i<12;i++)v[i]=g_motor_scope_meta[i];return 0;}
