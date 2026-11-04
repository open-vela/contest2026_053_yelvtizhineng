/* SPDX-License-Identifier: Apache-2.0
 * Explicit diagnostic only. PCNT2 observes GPIO6 TX and PCNT3 GPIO7 RX.
 * Register layout/actions verified against ESP-IDF5.5.4 pcnt_reg/pcnt_ll.
 * No UART registers, bytes, FIFO, clock, deadlines or output levels changed.
 * Only TX input-enable is borrowed, and all borrowed resources restored.
 */
#include <stdatomic.h>
#include "soc/pcnt_reg.h"
#include "soc/gpio_sig_map.h"
_Static_assert(CONFIG_ESP32S3_UART1_TXPIN==6 && CONFIG_ESP32S3_UART1_RXPIN==7,"UART1 diagnostic pin ownership");
_Static_assert(PCNT_CH0_NEG_MODE_U2_S==16 && PCNT_THR_H_LIM_EN_U2_S==12,"PCNT register layout");
_Static_assert(PCNT_SIG_CH0_IN2_IDX==41 && PCNT_SIG_CH0_IN3_IDX==45,"PCNT GPIO matrix signals");
_Static_assert(PCNT_U3_CONF0_REG==PCNT_U2_CONF0_REG+12 && PCNT_CLK_EN==(1u<<16),"PCNT unit layout");
static spinlock_t g_body_wire_lock=SP_UNLOCKED;
static _Atomic bool g_body_wire_on;
static struct {uint32_t clk,rst,ctrl,intr,conf[2][3],matrix[2],txpad;} g_body_wire_save;
#define BODY_WIRE_PAUSE ((1u<<5)|(1u<<7))
#define BODY_WIRE_CLEAR ((1u<<4)|(1u<<6))
#define BODY_WIRE_EVENTS 12u
static uint32_t body_wire_matrix(unsigned channel)
{return GPIO_FUNC0_IN_SEL_CFG_REG+4u*(channel?PCNT_SIG_CH0_IN3_IDX:PCNT_SIG_CH0_IN2_IDX);}
static uint32_t body_wire_conf(unsigned slot,unsigned word)
{return PCNT_U2_CONF0_REG+slot*12u+word*4u;}
static void body_wire_restore(void)
{
 modifyreg32(PCNT_CTRL_REG,0,BODY_WIRE_PAUSE);
 modifyreg32(PCNT_CTRL_REG,0,BODY_WIRE_CLEAR);modifyreg32(PCNT_CTRL_REG,BODY_WIRE_CLEAR,0);
 putreg32(BODY_WIRE_EVENTS,PCNT_INT_CLR_REG);
 for(unsigned s=0;s<2;s++)for(unsigned w=0;w<3;w++)putreg32(g_body_wire_save.conf[s][w],body_wire_conf(s,w));
 for(unsigned s=0;s<2;s++)putreg32(g_body_wire_save.matrix[s],body_wire_matrix(s));
 putreg32(g_body_wire_save.txpad,REG_IO_MUX_BASE+4u*7u);
 modifyreg32(PCNT_INT_ENA_REG,BODY_WIRE_EVENTS,g_body_wire_save.intr&BODY_WIRE_EVENTS);
 modifyreg32(PCNT_CTRL_REG,BODY_WIRE_PAUSE|BODY_WIRE_CLEAR|PCNT_CLK_EN,g_body_wire_save.ctrl&(BODY_WIRE_PAUSE|BODY_WIRE_CLEAR|PCNT_CLK_EN));
 modifyreg32(SYSTEM_PERIP_RST_EN0_REG,SYSTEM_PCNT_RST,g_body_wire_save.rst&SYSTEM_PCNT_RST);
 modifyreg32(SYSTEM_PERIP_CLK_EN0_REG,SYSTEM_PCNT_CLK_EN,g_body_wire_save.clk&SYSTEM_PCNT_CLK_EN);
}
int esp32s3_uart1_wire_counter_enabled(void){return g_body_wire_on?1:0;}
int esp32s3_uart1_wire_counter_config(bool enabled)
{
 irqstate_t flags=spin_lock_irqsave(&g_body_wire_lock);int rc=0;
 if(enabled==g_body_wire_on)goto done;
 if(enabled)
  {
   g_body_wire_save.clk=getreg32(SYSTEM_PERIP_CLK_EN0_REG);
   g_body_wire_save.rst=getreg32(SYSTEM_PERIP_RST_EN0_REG);
   g_body_wire_save.txpad=getreg32(REG_IO_MUX_BASE+4u*7u);
   for(unsigned s=0;s<2;s++)g_body_wire_save.matrix[s]=getreg32(body_wire_matrix(s));
   modifyreg32(SYSTEM_PERIP_CLK_EN0_REG,0,SYSTEM_PCNT_CLK_EN);
   modifyreg32(SYSTEM_PERIP_RST_EN0_REG,SYSTEM_PCNT_RST,0);
   g_body_wire_save.ctrl=getreg32(PCNT_CTRL_REG);g_body_wire_save.intr=getreg32(PCNT_INT_ENA_REG);
   for(unsigned s=0;s<2;s++)for(unsigned w=0;w<3;w++)g_body_wire_save.conf[s][w]=getreg32(body_wire_conf(s,w));
   if((g_body_wire_save.conf[0][0]&0xffff0000u) || (g_body_wire_save.conf[1][0]&0xffff0000u) ||
      (g_body_wire_save.intr&BODY_WIRE_EVENTS) || (getreg32(PCNT_INT_RAW_REG)&BODY_WIRE_EVENTS) ||
      (getreg32(PCNT_U2_CNT_REG)&0xffffu) || (getreg32(PCNT_U3_CNT_REG)&0xffffu))
    {modifyreg32(SYSTEM_PERIP_RST_EN0_REG,SYSTEM_PCNT_RST,g_body_wire_save.rst&SYSTEM_PCNT_RST);
     modifyreg32(SYSTEM_PERIP_CLK_EN0_REG,SYSTEM_PCNT_CLK_EN,g_body_wire_save.clk&SYSTEM_PCNT_CLK_EN);rc=-EBUSY;goto done;}
   modifyreg32(PCNT_CTRL_REG,0,BODY_WIRE_PAUSE|PCNT_CLK_EN);
   modifyreg32(PCNT_INT_ENA_REG,BODY_WIRE_EVENTS,0);
   for(unsigned s=0;s<2;s++)
    {putreg32((1u<<16)|(1u<<12),body_wire_conf(s,0)); /* falling increment, high-limit event, no filter */
     putreg32(0,body_wire_conf(s,1));putreg32(0x80007fffu,body_wire_conf(s,2));
     putreg32(0x80u+(s?7u:6u),body_wire_matrix(s));}
   putreg32(g_body_wire_save.txpad|FUN_IE,REG_IO_MUX_BASE+4u*7u);
   if(getreg32(body_wire_matrix(0))!=0x86u || getreg32(body_wire_matrix(1))!=0x87u ||
      getreg32(REG_IO_MUX_BASE+4u*7u)!=(g_body_wire_save.txpad|FUN_IE))
    {body_wire_restore();rc=-EIO;goto done;}
   g_body_wire_on=true;
  }
 else
  {g_body_wire_on=false;body_wire_restore();
   if(getreg32(REG_IO_MUX_BASE+4u*7u)!=g_body_wire_save.txpad ||
      getreg32(body_wire_matrix(0))!=g_body_wire_save.matrix[0] ||
      getreg32(body_wire_matrix(1))!=g_body_wire_save.matrix[1])rc=-EIO;}
done:spin_unlock_irqrestore(&g_body_wire_lock,flags);return rc;
}
int esp32s3_uart1_wire_counter_begin(void)
{
 irqstate_t flags=spin_lock_irqsave(&g_body_wire_lock);int rc=-ENOTSUP;
 if(g_body_wire_on)
  {modifyreg32(PCNT_CTRL_REG,0,BODY_WIRE_PAUSE);modifyreg32(PCNT_CTRL_REG,0,BODY_WIRE_CLEAR);
   modifyreg32(PCNT_CTRL_REG,BODY_WIRE_CLEAR,0);putreg32(BODY_WIRE_EVENTS,PCNT_INT_CLR_REG);
   modifyreg32(PCNT_CTRL_REG,BODY_WIRE_PAUSE,0);rc=0;}
 spin_unlock_irqrestore(&g_body_wire_lock,flags);return rc;
}
int esp32s3_uart1_wire_counter_end(int *tx,int *rx)
{
 if(!tx || !rx)return -EINVAL;
 irqstate_t flags=spin_lock_irqsave(&g_body_wire_lock);int rc=-ENOTSUP;
 if(g_body_wire_on)
  {modifyreg32(PCNT_CTRL_REG,0,BODY_WIRE_PAUSE);
   *tx=(int16_t)(getreg32(PCNT_U2_CNT_REG)&0xffffu);*rx=(int16_t)(getreg32(PCNT_U3_CNT_REG)&0xffffu);
   rc=(getreg32(PCNT_INT_RAW_REG)&BODY_WIRE_EVENTS)?-EOVERFLOW:(*tx<0 || *rx<0)?-ERANGE:0;}
 spin_unlock_irqrestore(&g_body_wire_lock,flags);return rc;
}
