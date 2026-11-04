/* UART1 only: bounded microsecond polling with interrupts left enabled.
 * Completion requires SW queue, FIFO and TX FSM empty. Never treats an event
 * or a timeout token as completion. No payload/IRQ/pin/clock/FIFO writes. */
#include <nuttx/mutex.h>
#include "esp32s3_rt_timer.h"
static mutex_t g_motor_txwire_lock=NXMUTEX_INITIALIZER;
static uint32_t g_motor_txwire_calls,g_motor_txwire_max_us;
static uint32_t g_motor_txwire_last_us,g_motor_txwire_timeouts;
static uint64_t g_motor_txwire_last_done;
extern void esp32s3_uart1_rx_usec_snapshot(uint64_t *received);
static uint32_t g_bus_guard_calls,g_bus_guard_max_us,g_bus_guard_last_rx_age;
static uint32_t g_bus_guard_last_tx_age,g_bus_guard_timeouts;
extern int esp32s3_uart1_diag_queue(uint32_t *v,unsigned count);
static int motor_txwire_init(unsigned id){(void)id;return 0;}
static uint32_t motor_txwire_keep(unsigned id){(void)id;return 0;}
static void motor_txwire_notify(unsigned id,uint32_t status){(void)id;(void)status;}
static bool motor_txwire_empty(const uint32_t v[10])
{return v[0]==v[1] && ((v[8]>>16)&0x3ff)==0 && ((v[9]>>4)&15)==0;}
int esp32s3_uart1_txwire_metrics(uint32_t v[4])
{
 if(!v)return -EINVAL;
 int rc=nxmutex_trylock(&g_motor_txwire_lock);if(rc<0)return rc;
 v[0]=g_motor_txwire_calls;v[1]=g_motor_txwire_max_us;
 v[2]=g_motor_txwire_last_us;v[3]=g_motor_txwire_timeouts;
 nxmutex_unlock(&g_motor_txwire_lock);return 0;
}
int esp32s3_uart1_txwire_wait(unsigned timeout_ms,bool *waited)
{
 if(!waited || !timeout_ms || timeout_ms>20)return -EINVAL;
 *waited=false;
 int rc=nxmutex_trylock(&g_motor_txwire_lock);if(rc<0)return rc;
 if(g_uart1_dev.open_count!=1){rc=g_uart1_dev.open_count?-EBUSY:-ENODEV;goto unlocked;}
 const unsigned budget=timeout_ms*1000;
 const uint64_t start=esp32s3_rt_timer_time_us();
 uint32_t v[10];uint64_t elapsed=0;
 g_motor_txwire_calls++;
 for(unsigned checks=0;;checks++)
   {
    rc=esp32s3_uart1_diag_queue(v,10);if(rc<0)break;
    elapsed=esp32s3_rt_timer_time_us()-start;
    if(elapsed>budget){rc=-ETIMEDOUT;break;}
    if(motor_txwire_empty(v)){g_motor_txwire_last_done=start+elapsed;rc=0;break;}
    *waited=true;
    if(elapsed>=budget || checks>=budget/5){rc=-ETIMEDOUT;break;}
    /* up_udelay is a busy microsecond delay, not tick-rounded usleep.
     * No lock/critical section is held: RX/TX IRQs and scheduling stay on. */
    unsigned left=budget-(unsigned)elapsed;
    up_udelay(left<5?left:5);
   }
 g_motor_txwire_last_us=elapsed>UINT32_MAX?UINT32_MAX:(uint32_t)elapsed;
 if(g_motor_txwire_last_us>g_motor_txwire_max_us)g_motor_txwire_max_us=g_motor_txwire_last_us;
 if(rc==-ETIMEDOUT)g_motor_txwire_timeouts++;
unlocked:
 nxmutex_unlock(&g_motor_txwire_lock);return rc;
}

/* Half-duplex transaction boundary. No new TX until both FSMs/FIFOs are
 * idle and at least1000us has elapsed since the previous TX and RX service.
 *1000us is one hundred8N1 characters at1Mbaud. IRQs stay enabled; no FIFO/clock/pin
 * writes, RX flushing or command retransmission occurs here. */
int esp32s3_uart1_bus_guard(unsigned timeout_ms)
{
 if(!timeout_ms || timeout_ms>20)return -EINVAL;
 int rc=nxmutex_trylock(&g_motor_txwire_lock);if(rc<0)return rc;
 if(g_uart1_dev.open_count!=1){rc=g_uart1_dev.open_count?-EBUSY:-ENODEV;goto out;}
 uint64_t start=esp32s3_rt_timer_time_us(),elapsed=0;unsigned budget=timeout_ms*1000;
 bool saw_tx_busy=false,saw_rx_busy=false,have_rx_mark=false;uint64_t rx_mark=0;
 g_bus_guard_calls++;
 for(unsigned checks=0;;checks++)
   {
    uint32_t q[10];uint64_t rx;
    rc=esp32s3_uart1_diag_queue(q,10);if(rc<0)break;
    esp32s3_uart1_rx_usec_snapshot(&rx);
    uint64_t now=esp32s3_rt_timer_time_us();elapsed=now-start;
    bool tx_idle=motor_txwire_empty(q),rx_idle=!(q[8]&0x3ff) && !(q[9]&15);
    if(!tx_idle)saw_tx_busy=true;
    else if(saw_tx_busy){g_motor_txwire_last_done=now;saw_tx_busy=false;}
    if(!rx_idle)saw_rx_busy=true;
    else if(saw_rx_busy){rx_mark=now;have_rx_mark=true;saw_rx_busy=false;}
    uint64_t rx_age=now-rx,tx_age=now-g_motor_txwire_last_done;
    if(have_rx_mark && now-rx_mark<rx_age)rx_age=now-rx_mark;
    if(elapsed>budget){rc=-ETIMEDOUT;break;}
    if(tx_idle && rx_idle && rx_age>=1000 && tx_age>=1000)
      {g_bus_guard_last_rx_age=rx_age>UINT32_MAX?UINT32_MAX:rx_age;
       g_bus_guard_last_tx_age=tx_age>UINT32_MAX?UINT32_MAX:tx_age;rc=0;break;}
    if(elapsed>=budget || checks>=budget/5){rc=-ETIMEDOUT;break;}
    unsigned left=budget-(unsigned)elapsed;up_udelay(left<5?left:5);
   }
 if(elapsed>g_bus_guard_max_us)g_bus_guard_max_us=elapsed>UINT32_MAX?UINT32_MAX:elapsed;
 if(rc==-ETIMEDOUT)g_bus_guard_timeouts++;
out:nxmutex_unlock(&g_motor_txwire_lock);return rc;
}
int esp32s3_uart1_bus_guard_metrics(uint32_t v[5])
{
 if(!v)return -EINVAL;
 int rc=nxmutex_trylock(&g_motor_txwire_lock);if(rc<0)return rc;
 v[0]=g_bus_guard_calls;v[1]=g_bus_guard_max_us;v[2]=g_bus_guard_last_rx_age;
 v[3]=g_bus_guard_last_tx_age;v[4]=g_bus_guard_timeouts;
 nxmutex_unlock(&g_motor_txwire_lock);return 0;
}
