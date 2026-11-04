/*
 * StackChan body: PY32 RGB/power, Si12T head touch, SCSCL feedback servos.
 * SPDX-License-Identifier: MIT
 * Portions based on Copyright (c) 2026 M5Stack Technology CO LTD
 * and Copyright (c) 2024 ftservo. See NOTICE.
 * Register/protocol reference: M5Stack StackChan firmware/main/hal (MIT).
 * Owns UART1 (/dev/ttyS1, TX6/RX7); no local soil UART is used.
 * All peripheral work runs here, never in the LVGL or voice callbacks.
 */
#include <nuttx/config.h>

#include <nuttx/clock.h>
#include <nuttx/i2c/i2c_master.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "stackchan_body.h"
#include "body_power_policy.h"
#include "body_ramp.h"
#include "body_sync_policy.h"
#include "body_servo_config.h"
#include "body_prepare_clock.h"
#include "body_loop_clock.h"
#define BODY_OFFICIAL_FLOW_PRESENT 1
/* Production: timeout diagnostic probes are not compiled or armed. */
static int g_servo_reply_level[2]={-1,-1};
static unsigned g_servo_min[2],g_servo_max[2]={1000,1000};
static bool body_servo_target_valid(int id,int target)
{return body_raw_valid(id,target) && target>=(int)g_servo_min[id-1] && target<=(int)g_servo_max[id-1];}

/* Explicit diagnostic only. Never commits or continues after absent ACK. */
#define BODY_ACKPROBE_PENDING 1
static _Atomic bool g_ackprobe_enabled=true;
static unsigned g_goal_retry_mask; /* One recovery round per original frame. */
static _Atomic unsigned g_ackprobe_wire_id,g_ackprobe_events,g_ackprobe_valid_mask,g_ackprobe_applied;
static _Atomic int g_ackprobe_id,g_ackprobe_expected,g_ackprobe_observed;
static _Atomic int g_ackprobe_goal_rc,g_ackprobe_health_rc0,g_ackprobe_health_rc1;
static _Atomic unsigned g_ackprobe_elapsed;
static struct body_sync_window g_sync_window;
static unsigned g_stage_acked_mask;
static _Atomic unsigned g_sync_committed_frames,g_sync_verified_goals,g_sync_health_reads,g_sync_verified_off,g_sync_failed_frames;
static _Atomic unsigned g_goal_staged,g_goal_applied,g_goal_unchanged_verified,g_goal_write_packets,g_goal_repeat_frames,g_goal_repeat_targets,g_goal_repeat_recovered;
#include "../../hal/hal_i2c.h"
#ifdef CONFIG_PLANT_AI_VOICE
#include "../../services/voice_service.h"
#endif

static uint8_t g_py_addr = 0x6f;
#define PY32 g_py_addr
#define SI12 0x68
#define BODY_FREQ 100000
#define BODY_TICK_US 20000

extern struct i2c_master_s *esp32s3_i2cbus_initialize(int bus);
static struct i2c_master_s *g_bus;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool g_running;
static _Atomic bool g_sleeping,g_idle_done;
static _Atomic int g_power_state=BODY_OFF_IDLE;
static _Atomic bool g_prepare_pending,g_prepare_done,g_prepare_cancel;
/* Explicit one-shot observer request: never enables power or motion. */
static _Atomic bool g_observe_armed;
#include "body_observe_policy.h"
static _Atomic int g_prepare_rc;
static _Atomic int g_idle_rc;
static _Atomic bool g_started;
static _Atomic bool g_py_ok;
static _Atomic bool g_touch_ok;
static _Atomic bool g_servo_ok;
static _Atomic bool g_auto = true;
static _Atomic bool g_motion; /* Enabled only after both valid position replies. */
static int g_uart = -1;
static int g_touch_event;
static _Atomic unsigned g_presses;
static _Atomic unsigned g_releases;
static _Atomic unsigned g_i2c_errors;
static _Atomic unsigned g_uart_errors;
static _Atomic unsigned g_light_updates;
static _Atomic unsigned g_moves;
static _Atomic unsigned g_motor_faults;
static unsigned g_peak_load[2],g_peak_current[2]; /* Worker-owned move telemetry. */
static int g_feedback_rc[2];
static uint32_t now_ms(void);
struct body_sample {uint32_t at;unsigned pos,voltage,temperature,load,current;};
static struct body_sample g_samples[2][8];
static unsigned g_sample_count[2];
static void body_sample_record(int id,const uint8_t *p)
{
 unsigned slot=(unsigned)(id-1);struct body_sample *v=&g_samples[slot][g_sample_count[slot]++%8];
 v->at=now_ms();v->pos=((unsigned)p[0]<<8)|p[1];
 v->voltage=p[6];v->temperature=p[7];
 v->load=(((unsigned)p[4]<<8)|p[5])&0x3ff;
 v->current=(((unsigned)p[13]<<8)|p[14])&0x7fff;
}
static void body_samples_dump(void)
{
 for(unsigned i=0;i<2;i++)for(unsigned n=g_sample_count[i]>8?g_sample_count[i]-8:0;n<g_sample_count[i];n++)
  {struct body_sample *v=&g_samples[i][n%8];
   printf("[BodySample] id=%u n=%u at=%u pos=%u voltage_raw=%u temperature=%u load=%u current=%u\n",i+1,n,v->at,v->pos,v->voltage,v->temperature,v->load,v->current);}
}
static unsigned g_feedback_load[2],g_feedback_current[2];
static int g_feedback_position[2];
static _Atomic unsigned g_version;
static _Atomic unsigned g_touch_raw;
static _Atomic unsigned g_bmi_id;
static _Atomic unsigned g_uid;
static _Atomic int g_pos[2] = {-1, -1};
static _Atomic int g_home[2] = {BODY_HOME_YAW, BODY_HOME_PITCH};
static _Atomic int g_voice;
static unsigned g_request;
static _Atomic bool g_feedback_test;
static _Atomic bool g_feedback_cancel;
static bool g_feedback_powered; /* g_lock protects request publication. */
static uint8_t g_rgb[3];
static _Atomic bool g_focused;
static _Atomic bool g_focus_hold=false; /* Official Hal::startXiaozhi enables auto torque release. */
static _Atomic bool g_power_trace;
static _Atomic bool g_settled=true;
static int g_diag_axis; /* g_lock publishes explicit isolated-axis requests. */
static int g_target_yaw;
static int g_target_pitch;

enum { REQ_AXIS = 2097152, REQ_BUS_CHECK = 1048576, REQ_STARTUP = 524288, REQ_LED = 1, REQ_PROBE = 2, REQ_MOVE = 4, REQ_NOD = 8,
       REQ_STOP = 16, REQ_AUTO = 32, REQ_POWER_OFF = 64, REQ_RELEASE = 128, REQ_DIAG = 256, REQ_POSE = 512, REQ_FEEDBACK_TEST = 1024, REQ_UART_TEST = 2048, REQ_UART_WAVE = 4096, REQ_UART_RECEIVE = 8192, REQ_UART_COMPARE = 16384, REQ_LED_BUS = 32768, REQ_IDLE_POWER = 65536, REQ_PREPARE = 131072, REQ_POWER_PATH = 262144 };

static uint32_t now_ms(void)
{
  return TICK2MSEC(clock_systime_ticks());
}

static int rd(uint8_t addr, uint8_t reg, uint8_t *value)
{
  int ret = hal_i2c_read_reg(g_bus, addr, reg, value, BODY_FREQ);
  if (ret < 0) { g_i2c_errors++; printf("[BodyRegError] op=read addr=%02x reg=%02x rc=%d\n",addr,reg,ret); }
  return ret;
}

static int wr(uint8_t addr, uint8_t reg, uint8_t value)
{
  int ret = hal_i2c_write_reg(g_bus, addr, reg, value, BODY_FREQ);
  if (ret < 0) { g_i2c_errors++; printf("[BodyRegError] op=write addr=%02x reg=%02x rc=%d\n",addr,reg,ret); }
  return ret;
}

extern int esp32s3_uart1_park_tx(void);
extern void esp32s3_uart1_resume_pins(void);
extern int esp32s3_uart1_pins_parked(void);

static uint32_t g_fault_pin_snapshot[18];
static bool g_fault_pin_snapshot_seen;
static int g_fault_pin_snapshot_rc=-ENODATA;
extern int esp32s3_uart1_faultpins_capture(uint32_t *,unsigned) __attribute__((weak));
static bool g_fault_vm_seen;static unsigned g_fault_vm_latch;
static int bit(uint8_t reg, uint8_t mask, bool on)
{
  uint8_t v;
  /* Cut the signal drive before disabling VM. Never delay the safety cut
   * if parking fails; still attempt the mandatory power write/readback. */
  /* Sample registers before park changes the pin state. This does not
   * wait or send traffic and never conditions/delays the mandatory cut. */
  if(!on && reg==0x05 && (mask&1) && g_power_state==BODY_FAULTED &&
     !g_fault_pin_snapshot_seen && esp32s3_uart1_faultpins_capture)
    {g_fault_pin_snapshot_seen=true;
     g_fault_pin_snapshot_rc=esp32s3_uart1_faultpins_capture(g_fault_pin_snapshot,18);}
  int parked=(!on && reg==0x05 && (mask&1))?esp32s3_uart1_park_tx():0;
  int ret = rd(PY32, reg, &v);
  if (ret < 0) return ret;
  /* Retain the existing pre-cut read. No extra I2C or delay before cut. */
  if(!on && reg==0x05 && (mask&1) && g_power_state==BODY_FAULTED && !g_fault_vm_seen)
    {g_fault_vm_latch=v&1;g_fault_vm_seen=true;}
  ret=wr(PY32, reg, on ? v | mask : v & ~mask);
  return ret<0?ret:parked<0?parked:ret;
}

static int leds(uint8_t r, uint8_t g, uint8_t b)
{
  uint16_t color = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
  uint8_t data[25];
  struct i2c_msg_s msg;
  int i;
  int ret;
  if (!g_py_ok) return -ENODEV;
  data[0] = 0x30;
  for (i = 0; i < 12; i++)
    {
      data[1 + i * 2] = color & 0xff;
      data[2 + i * 2] = color >> 8;
    }
  memset(&msg, 0, sizeof(msg));
  msg.frequency = BODY_FREQ;
  msg.addr = PY32;
  msg.buffer = data;
  msg.length = sizeof(data);
  ret = I2C_TRANSFER(g_bus, &msg, 1);
  if (ret < 0) { g_i2c_errors++; return ret; }
  ret = wr(PY32, 0x24, 12 | 0x40);
  if (ret >= 0)
    {
      g_light_updates++;
    }
  return ret;
}

/* Bounded packet reader: validates ID, length, checksum and servo error.
 * A transmitted instruction or UART echo is never accepted as feedback. */
extern void esp32s3_uart1_rx_snapshot(uint32_t *received_ms,
                                       uint32_t *sequence);
static _Atomic unsigned g_uart_buffered_ontime;
static _Atomic unsigned g_uart_late_rejected;
static _Atomic unsigned g_uart_parse_expired;

/* Vendor wFlushSCS waits for the wire, not only the software queue.
 * Bound that wait by20ms and leave the existing60/100ms RX limits intact.
 * The queue snapshot is read-only and uses the same UART spinlock as ISR. */
extern int esp32s3_uart1_diag_queue(uint32_t *v,unsigned count);
extern uint32_t esp32s3_uart1_rx_error_begin(void);
static unsigned g_uart_tx_waits,g_uart_tx_timeouts;
static uint32_t g_uart_last_fault[10];
static int g_uart_last_rx_used,g_uart_last_reg;
extern int esp32s3_uart1_txwire_wait(unsigned timeout_ms,bool *waited);
static int body_uart_tx_drain_budget(unsigned budget)
{
  bool waited=false;int rc=esp32s3_uart1_txwire_wait(budget,&waited);
  if(waited)g_uart_tx_waits++;
  if(rc<0)
    {if(rc==-ETIMEDOUT)g_uart_tx_timeouts++;
     esp32s3_uart1_diag_queue(g_uart_last_fault,10);}
  return rc;
}
static int body_uart_tx_drain(void){return body_uart_tx_drain_budget(20);}

/* Bounded numeric transaction history. Never retains PCM or user data. */
extern int esp32s3_uart1_diag_snapshot(uint32_t *v,unsigned count);
extern int esp32s3_uart1_txmirror_config(bool enabled);
extern int esp32s3_uart1_txmirror_begin(void);
extern int esp32s3_uart1_rxmirror_end(unsigned id,unsigned payload,int v[4]);
extern void esp32s3_uart1_rxmirror_observe(void);
static _Atomic bool g_txmirror_enabled;
static _Atomic bool g_txproof_enabled;
extern int esp32s3_uart1_txproof_config(bool on);
extern int esp32s3_uart1_txproof_rearm_input(void);
extern int esp32s3_uart1_txmirror_end(const uint8_t *p,unsigned n,int v[4]);
extern int esp32s3_uart1_wire_counter_config(bool enabled);
extern int esp32s3_uart1_wire_counter_enabled(void);
extern int esp32s3_uart1_wire_counter_begin(void);
extern int esp32s3_uart1_wire_counter_end(int *tx,int *rx);
struct body_bus_trace {uint32_t at,elapsed,before[10],after[10];int id,ins,reg,rc,used,wire_rc,tx_edges,rx_edges,expected_tx,expected_rx,mirror_rc,mirror_count,mirror_match,mirror_errors,mirror_first_diff;};
static struct body_bus_trace g_bus_trace[16];
static unsigned g_bus_trace_count;
static pthread_mutex_t g_wire_view_lock=PTHREAD_MUTEX_INITIALIZER;
static struct body_bus_trace g_wire_lastview;
static unsigned g_wire_view_number;
static bool g_wire_view_ready;

static unsigned body_wire_byte_edges(uint8_t byte)
{unsigned n=1;for(unsigned i=0;i<7;i++)if((byte&(1u<<i)) && !(byte&(1u<<(i+1))))n++;return n;}
static unsigned body_wire_packet_edges(uint8_t id,uint8_t code,const uint8_t *data,int count)
{unsigned sum=id+(count+2)+code;unsigned edges=2*body_wire_byte_edges(255)+body_wire_byte_edges(id)+body_wire_byte_edges(count+2)+body_wire_byte_edges(code);
 for(int i=0;i<count;i++){sum+=data[i];edges+=body_wire_byte_edges(data[i]);}
 return edges+body_wire_byte_edges((uint8_t)~sum);}
static void body_wire_fault_summary(void)
{if(g_bus_trace_count){struct body_bus_trace *t=&g_bus_trace[(g_bus_trace_count-1)%16];
 printf("[BodyWire] n=%u id=%d ins=%d reg=%d UART_rc=%d counter_rc=%d tx_edges=%d expected_tx=%d rx_edges=%d expected_rx=%d\n",g_bus_trace_count-1,t->id,t->ins,t->reg,t->rc,t->wire_rc,t->tx_edges,t->expected_tx,t->rx_edges,t->expected_rx);}}
static void body_bus_fault_trace(void)
{
  unsigned first=g_bus_trace_count>16?g_bus_trace_count-16:0;
  for(unsigned n=first;n<g_bus_trace_count;n++)
    {struct body_bus_trace *t=&g_bus_trace[n%16];
     printf("[BodyBusTrace] n=%u at=%u ms=%u id=%d ins=%d reg=%d rc=%d used=%d rx_before=%u rx_after=%u ena_before=%08x ena_after=%08x raw_before=%08x raw_after=%08x status=%08x conf1=%08x\n",
      n,t->at,t->elapsed,t->id,t->ins,t->reg,t->rc,t->used,t->before[2],t->after[2],t->before[5],t->after[5],t->before[7],t->after[7],t->after[8],t->after[4]);}
}
static void body_ramps_cancel(void);
/* All production requests share this fence, including torque-OFF, stop,
 * touch, settle and nod paths. A first failed transaction cuts power before
 * publishing diagnostics; later callers may not emit another UART packet.
 * OFF-state diagnostic windows retain their own existing power-off cleanup.
 */
extern int esp32s3_uart1_scope_freeze_after_cut(void);
static int body_transport_fail_stop(void)
{
  g_stage_acked_mask=0;
  g_motor_faults++;g_power_state=BODY_FAULTED;
  g_servo_ok=false;g_motion=false;g_settled=false;
  body_ramps_cancel();
  int off=bit(0x05,1,false);uint8_t rail=0xff;
  int verify=rd(PY32,0x05,&rail);
  if(off>=0 && verify>=0 && !(rail&1))(void)esp32s3_uart1_scope_freeze_after_cut();
  printf("[BodyPreCutVM] valid=%u VM_EN_output_latch=%u extra_I2C_before_cut=0 physical_voltage_not_measured=1\n",g_fault_vm_seen,g_fault_vm_latch);
  return off>=0 && verify>=0 && !(rail&1);
}
extern int esp32s3_uart1_timing_config(bool on);
extern int esp32s3_uart1_timing_enabled(void);
extern int esp32s3_uart1_timing_begin(unsigned number,unsigned id,unsigned length);
extern int esp32s3_uart1_timing_end(int rc);
/* Sole body worker scopes this flag around the official periodic pitch READ. */
static bool g_hal_feedback_periodic;
static int servo_packet_traced(uint8_t id,uint8_t instruction,const uint8_t *params,int count,uint8_t *reply,int reply_count);
static int servo_packet(uint8_t id,uint8_t instruction,const uint8_t *params,int count,uint8_t *reply,int reply_count)
{
  if(g_power_state==BODY_FAULTED)return -EHOSTDOWN;
  struct body_bus_trace *t=&g_bus_trace[g_bus_trace_count++%16];
  memset(t,0,sizeof(*t));t->at=now_ms();t->id=id;t->ins=instruction;t->reg=count?params[0]:-1;
  esp32s3_uart1_diag_queue(t->before,10);
  t->tx_edges=t->rx_edges=t->expected_rx=-1;t->expected_tx=body_wire_packet_edges(id,instruction,params,count);
  t->wire_rc=esp32s3_uart1_wire_counter_begin();
  t->mirror_rc=-ENOTSUP;t->mirror_count=-1;t->mirror_match=0;t->mirror_errors=0;t->mirror_first_diff=-1;
  if((g_txmirror_enabled || g_txproof_enabled) && id>=1 && id<=2 && reply_count>=0 && reply_count<=58)
    t->mirror_rc=esp32s3_uart1_txmirror_begin();
  int rc=servo_packet_traced(id,instruction,params,count,reply,reply_count);
  int rail_off=-1;
#ifdef BODY_TIMEOUT_DIAGNOSTIC
  if(rc==-ETIMEDOUT && instruction==2 && reply_count==15 && count==2 && params && params[0]==56)
    if(!g_txproof_enabled)body_timeout_diagnose(id);
#endif
  if(rc<0 && !(rc==-ENODATA && g_hal_feedback_periodic && id==2 && instruction==2 && count==2 && params && params[0]==56 && params[1]==15 && reply_count==15) && (g_power_state==BODY_READY || g_power_state==BODY_PREPARING))
    rail_off=body_transport_fail_stop();
  (void)esp32s3_uart1_timing_end(rc);
  if(t->mirror_rc==0)
    {int v[4]={-1,0,0,-1};if(g_txproof_enabled){uint8_t expected[24];unsigned sum=id+count+2+instruction;
       expected[0]=255;expected[1]=255;expected[2]=id;expected[3]=count+2;expected[4]=instruction;
       for(int i=0;i<count;i++){expected[5+i]=params[i];sum+=params[i];}expected[5+count]=(uint8_t)~sum;
       t->mirror_rc=esp32s3_uart1_txmirror_end(expected,count+6,v);
      }else t->mirror_rc=esp32s3_uart1_rxmirror_end(id,reply_count,v);
     t->mirror_count=v[0];t->mirror_match=v[1];t->mirror_errors=v[2];t->mirror_first_diff=v[3];}
  if(t->wire_rc==0)t->wire_rc=esp32s3_uart1_wire_counter_end(&t->tx_edges,&t->rx_edges);
  if(rc==0)t->expected_rx=((id==0xfe && instruction==0x83) || (id>=1 && id<=2 && (instruction==3 || instruction==4) && g_servo_reply_level[id-1]==0))?0:body_wire_packet_edges(id,0,reply,reply_count);
  t->elapsed=now_ms()-t->at;t->rc=rc;t->used=rc==BODY_ACKPROBE_PENDING?0:rc<0?g_uart_last_rx_used:(((id==0xfe && instruction==0x83) || (id>=1 && id<=2 && (instruction==3 || instruction==4) && g_servo_reply_level[id-1]==0))?0:reply_count+6);
  esp32s3_uart1_diag_queue(t->after,10);
  pthread_mutex_lock(&g_wire_view_lock);g_wire_lastview=*t;
  g_wire_view_number=g_bus_trace_count-1;g_wire_view_ready=true;pthread_mutex_unlock(&g_wire_view_lock);
#ifdef BODY_TIMEOUT_DIAGNOSTIC
  if(rail_off>=0)body_timeout_diagnostic_print();
#endif
  if(rail_off>=0)printf("[BodyTransportStop] id=%u rc=%d rail_off=%d no_uart_retry_after_fault=1\n",id,rc,rail_off);
  return rc;
}

extern int esp32s3_uart1_scope_config(bool on);
extern int esp32s3_uart1_scope_trigger(unsigned number,unsigned id,unsigned reg,unsigned length);
extern int esp32s3_uart1_scope_freeze_after_cut(void);
extern int esp32s3_uart1_scope_status(uint32_t *v,unsigned n);
extern int esp32s3_uart1_halfduplex_tx(bool drive);
extern int esp32s3_uart1_halfduplex_metrics(uint32_t *v,unsigned n);
static _Atomic unsigned g_read_repeats,g_read_recovered;
static int servo_packet_traced(uint8_t id, uint8_t instruction,
                        const uint8_t *params, int count,
                        uint8_t *reply, int reply_count)
{
  unsigned read_repeated=0;
  bool periodic=g_hal_feedback_periodic && id==2 && instruction==2 && count==2 && params && params[0]==56 && params[1]==15 && reply_count==15;
  uint8_t tx[24];
  uint8_t rx[64];
  unsigned sum;
  int used = 0;
  int total = count + 6;
  int i;
  uint32_t start, baseline_ms, baseline_sequence;
  if(g_uart<0 || count<0 || count>16 || (count>10 && !((id==0xfe && instruction==0x83) || (id>=1 && id<=2 && (instruction==3 || instruction==4) && g_servo_reply_level[id-1]==0))))return -ENODEV;
  bool sync=id==0xfe && instruction==0x83;
  if(instruction==5)
    {
      if(id<1 || id>2 || count || params || reply || reply_count ||
         !g_sync_window.active || g_servo_reply_level[id-1]!=1 ||
         !(g_stage_acked_mask&(1u<<(id-1))))return -EINVAL;
      /* Single-use permission: even a failed TX cannot repeat ACTION. */
      g_stage_acked_mask&=~(1u<<(id-1));
    }

  /* Before reading config, the only allowed unknown-config WRITE is OFF.
   * Require its complete ACK just like the official default Level=1 SDK.
   * Position/EEPROM/torque-ON remain forbidden without validated config. */
  bool initial_off=id>=1 && id<=2 && instruction==3 && count==2 && params &&
                   params[0]==40 && params[1]==0 && g_servo_reply_level[id-1]==-1;
  bool no_ack=id>=1 && id<=2 && (instruction==3 || instruction==4) && g_servo_reply_level[id-1]==0;
  if((instruction==3 || instruction==4) && (!g_sync_window.active || id<1 || id>2 ||
     (g_servo_reply_level[id-1]<0 && !initial_off) || g_servo_reply_level[id-1]>1 ||
     !body_unicast_payload_valid(id,params,count)))return -EINVAL;
  if(instruction==4 && params[0]!=42)return -EINVAL;
  if((instruction==3 || instruction==4) && params[0]==42 && !body_servo_target_valid(id,((unsigned)params[1]<<8)|params[2]))return -ERANGE;

  if((id==0xfe || instruction==0x83) && (!sync || !g_sync_window.active || reply_count || !body_sync_payload_valid(params,count)))return -EINVAL;
  tx[0] = 0xff; tx[1] = 0xff; tx[2] = id;
  tx[3] = count + 2; tx[4] = instruction;
  sum = id + tx[3] + instruction;
  for (i = 0; i < count; i++) { tx[5 + i] = params[i]; sum += params[i]; }
  tx[5 + count] = (uint8_t)~sum;
  start=now_ms();
  unsigned arrival_limit=body_sync_budget(&g_sync_window,start,60);
  unsigned parse_limit=body_sync_budget(&g_sync_window,start,100);
#ifdef BODY_TIMEOUT_DIAGNOSTIC
  if(g_timeout_diag_active)
    {unsigned elapsed=(uint32_t)(now_ms()-g_timeout_diag_start);
     if(elapsed>=BODY_TIMEOUT_DIAG_BUDGET_MS)return -ETIMEDOUT;
     unsigned left=BODY_TIMEOUT_DIAG_BUDGET_MS-elapsed;
     if(arrival_limit>left)arrival_limit=left;if(parse_limit>left)parse_limit=left;}
#endif
  if(!arrival_limit || !parse_limit)return -ETIMEDOUT;
  extern int esp32s3_uart1_bus_guard(unsigned timeout_ms);
  int quiet=esp32s3_uart1_bus_guard(arrival_limit<20?arrival_limit:20);
  if(quiet<0){g_uart_errors++;g_uart_last_rx_used=0;g_uart_last_reg=count?params[0]:-1;esp32s3_uart1_diag_queue(g_uart_last_fault,10);return quiet;}
  int direction_rc=esp32s3_uart1_halfduplex_tx(true);
  if(direction_rc<0)return direction_rc;
  if (tcflush(g_uart, TCIFLUSH) < 0) return -EIO;
  (void)esp32s3_uart1_rx_error_begin();
  esp32s3_uart1_rx_snapshot(&baseline_ms, &baseline_sequence);
  unsigned spent=(uint32_t)(now_ms()-start);
  if(spent>=arrival_limit)return -ETIMEDOUT;
  /* NuttX uart_write disables TX IRQ until its entire argument is queued.
   * UART1 has256 TX bytes and the idle guard precedes this <=22-byte frame.
   * Submit ONE complete frame: separate writes can emit a header and then
   * leave an application scheduling gap before data/checksum. Bytes, frame
   * limits and all ACK/readback/error checks remain unchanged; no retry. */
  if(id>=1 && id<=2 && count==7 && params[0]==42 && (instruction==3 || instruction==4))
    (void)esp32s3_uart1_timing_begin(g_bus_trace_count-1,id,total);
  /* Diagnostic only, default OFF: preserve the complete target frame and
   * first READ interval. No rearm on retries, no observer-based acceptance. */
  if(sync)
    (void)esp32s3_uart1_scope_trigger(g_bus_trace_count-1,id,params[0],count);
  spent=(uint32_t)(now_ms()-start);if(spent>=arrival_limit)return -ETIMEDOUT;
  if(write(g_uart,tx,total)!=total)return -EIO;
  spent=(uint32_t)(now_ms()-start);if(spent>=arrival_limit)return -ETIMEDOUT;
  unsigned left=arrival_limit-spent;
  unsigned tx_budget=left<20?left:20;
  int tx_rc=body_uart_tx_drain_budget(tx_budget);
  if(tx_rc<0){g_uart_errors++;g_uart_last_rx_used=0;g_uart_last_reg=count?params[0]:-1;return tx_rc;}
  if(id>=1 && id<=2 && instruction==3 && count==7 && params && params[0]==42)
    {extern void esp32s3_uart1_goal_stamp(unsigned,uint32_t) __attribute__((weak));
     if(esp32s3_uart1_goal_stamp)esp32s3_uart1_goal_stamp(id,now_ms());}
  direction_rc=esp32s3_uart1_halfduplex_tx(false);
  if(direction_rc<0)return direction_rc;
  uint32_t ack_probe_started=now_ms();
  /* Official SCSerial IOTimeOut is10ms per byte. For an empty response
   * use10ms from completed TX; retain the outer60/100ms hard bounds. */
  uint32_t periodic_progress=ack_probe_started;
  if(sync || no_ack)return 0; /* Protocol-declared no write ACK; caller still requires exact readback. */
  for (;;)
    {
      uint32_t elapsed = (uint32_t)(now_ms() - start);
      if(elapsed<=arrival_limit)esp32s3_uart1_rxmirror_observe();
      elapsed=(uint32_t)(now_ms()-start); /*Observer cost uses the original budget.*/
      uint32_t received_ms, sequence, parsed_ms;
      if (elapsed > parse_limit)
        { g_uart_parse_expired++; break; }
      /* After the 60ms arrival deadline, inspect already-buffered bytes
       * once without blocking. A late task may use only on-time feedback,
       * and must consume it within the existing 100ms feedback period.
       * This does not extend the servo's response deadline.
       */
      int timeout = elapsed < arrival_limit ? (int)(arrival_limit - elapsed) : 0;
      if (timeout > 5) timeout = 5;
      extern int esp32s3_uart1_rxpoll_service(void);
      int service_rc=esp32s3_uart1_rxpoll_service();
      if(service_rc<0)return service_rc;
      if(service_rc>0)timeout=0; /* Manual RX service cannot sleep waiting for its masked IRQ. */
      struct pollfd pfd = { .fd = g_uart, .events = POLLIN };
      if (poll(&pfd, 1, timeout) > 0)
        {
          int n = read(g_uart, rx + used, sizeof(rx) - used);
          if (n > 0){used += n;periodic_progress=now_ms();}
        }
      if((uint32_t)(now_ms()-start)<=arrival_limit)esp32s3_uart1_rxmirror_observe();
      esp32s3_uart1_rx_snapshot(&received_ms, &sequence);
      parsed_ms = now_ms();
      for (i = 0; i + reply_count + 6 <= used; i++)
        {
          int j;
          int length = reply_count + 6;
          if (rx[i] != 0xff || rx[i + 1] != 0xff ||
              rx[i + 2] != id || rx[i + 3] != reply_count + 2) continue;
          if (length == total && !memcmp(rx + i, tx, total)) continue;
          sum = 0;
          for (j = 2; j < length; j++) sum += rx[i + j];
          if ((sum & 0xff) != 0xff) continue;
          /* The latest RX-service stamp may include trailing noise. That
           * conservatively rejects the packet, rather than accepting any
           * byte whose timeliness cannot be established. Same-ID stale
           * replies remain an inherent protocol limitation (no nonce).
           */
          if (sequence == baseline_sequence ||
              (uint32_t)(received_ms - start) >= arrival_limit ||
              (uint32_t)(parsed_ms - start) > parse_limit)
            { g_uart_late_rejected++; continue; }
          if (rx[i + 4] != 0) return -EIO;
          if (reply_count) memcpy(reply, rx + i + 5, reply_count);
          if ((uint32_t)(parsed_ms - start) >= arrival_limit) g_uart_buffered_ontime++;
          if(read_repeated)g_read_recovered++;
          return 0;
        }
      /* READ has no actuator side effects. A completely absent response may
       * be requested at10/30ms, at most twice more, only before the SAME original60ms deadline.
       * Keep the original RX baseline/start, no flush, no window renewal.
       * Partial/invalid replies, writes, OFF, ACTION and faulted-state calls
       * are never retried. No new target can be committed until exact goal
       * and both fresh health samples pass in that same transaction window. */
      if(!periodic && read_repeated<2 && !used && instruction==2 && id>=1 && id<=2 &&
         count==2 && params && reply_count>0 && params[1]==reply_count &&
         g_sync_window.active && (uint32_t)(parsed_ms-start)>=(read_repeated?30u:10u) &&
         (uint32_t)(parsed_ms-start)<arrival_limit &&
         arrival_limit-(uint32_t)(parsed_ms-start)>10)
        {
          /* Scope is armed before the preceding SyncWrite, not this repeat. */
          unsigned remaining=arrival_limit-(uint32_t)(parsed_ms-start);
          int guard=esp32s3_uart1_bus_guard(remaining<20?remaining:20);
          if(guard<0)return guard;
          unsigned consumed=(uint32_t)(now_ms()-start);
          if(consumed>=arrival_limit)return -ETIMEDOUT;
          direction_rc=esp32s3_uart1_halfduplex_tx(true);
          if(direction_rc<0)return direction_rc;
          if((uint32_t)(now_ms()-start)>=arrival_limit)return -ETIMEDOUT;
          if(write(g_uart,tx,total)!=total)return -EIO;
          consumed=(uint32_t)(now_ms()-start);
          if(consumed>=arrival_limit)return -ETIMEDOUT;
          unsigned budget=arrival_limit-consumed;
          int drained=body_uart_tx_drain_budget(budget<20?budget:20);
          if(drained<0)return drained;
          direction_rc=esp32s3_uart1_halfduplex_tx(false);
          if(direction_rc<0)return direction_rc;
          read_repeated++;g_read_repeats++;
        }
      /* Only a completed TX followed by zero received bytes is feedback
       * unavailable. TX/drain errors and partial/malformed packets remain
       * hard failures. No additional READ or motor command is issued. */
      if(periodic && (uint32_t)(parsed_ms-periodic_progress)>=10)
        {if(!used){esp32s3_uart1_rx_snapshot(&received_ms,&sequence);
          if(sequence==baseline_sequence){g_uart_last_rx_used=0;g_uart_last_reg=56;return -ENODATA;}}
         break;}
      uint32_t probe_elapsed=(uint32_t)(now_ms()-start);
      if(g_ackprobe_wire_id==id && instruction==3 && id>=1 && id<=2 &&
         count==7 && params && params[0]==42 && !reply_count && !used &&
         g_sync_window.active && g_servo_reply_level[id-1]==1 &&
         (uint32_t)(now_ms()-ack_probe_started)>=10)
        {
          /* Service before diagnosing absence. No new UART command. */
          int pending_service=esp32s3_uart1_rxpoll_service();
          if(pending_service<0)return pending_service;
          esp32s3_uart1_rx_snapshot(&received_ms,&sequence);
          if(sequence==baseline_sequence)return BODY_ACKPROBE_PENDING;
        }
      if ((uint32_t)(parsed_ms - start) >= arrival_limit || used == sizeof(rx)) break;
    }
  /* Official HAL ignores an absent Ack result. The shared frame budget
   * may expire before the per-call10ms wait: after completed TX this is
   * still UNCONFIRMED, not a hard write/driver failure. Never accept any
   * received malformed data, failed TX, or terminal READ through this path. */
  if(g_ackprobe_wire_id==id && instruction==3 && id>=1 && id<=2 &&
     count==7 && params && params[0]==42 && !reply_count && !used &&
     g_sync_window.active && g_servo_reply_level[id-1]==1)
    {int service=esp32s3_uart1_rxpoll_service();if(service<0)return service;
     uint32_t at,sequence;esp32s3_uart1_rx_snapshot(&at,&sequence);
     if(sequence==baseline_sequence)return BODY_ACKPROBE_PENDING;}
  esp32s3_uart1_diag_queue(g_uart_last_fault,10);
  g_uart_last_rx_used=used;g_uart_last_reg=count?params[0]:-1;
  g_uart_errors++;
  return -ETIMEDOUT;
}

#ifdef BODY_TIMEOUT_DIAGNOSTIC
#include "body_timeout_diagnostic.inc"
#endif

static int servo_read(uint8_t id, uint8_t reg, uint8_t *data, int len)
{
  uint8_t p[2] = {reg, len};
  return servo_packet(id, 2, p, 2, data, len);
}

static int position(int id)
{
  uint8_t v[2];
  int ret = servo_read(id, 56, v, 2);
  if (ret < 0) return ret;
  ret = ((int)v[0] << 8) | v[1]; /* Official SCSCL End=1, big endian. */
  return ret <= 1000 ? ret : -ERANGE;
}

static int servo_write(uint8_t id, uint8_t reg, const uint8_t *v, int len)
{
  uint8_t p[10];
  if (len > 9) return -EINVAL;
  p[0] = reg;
  memcpy(p + 1, v, len);
  return servo_packet(id, 3, p, len + 1, NULL, 0);
}

static int servo_stage(uint8_t id,const uint8_t data[6])
{
 uint8_t p[7];p[0]=42;memcpy(p+1,data,6);
 return servo_packet(id,4,p,7,NULL,0);
}
static int servo_apply(uint8_t id)
{return servo_packet(id,5,NULL,0,NULL,0);}

static int body_sync_off(unsigned mask);
static int body_sync_move(unsigned mask,const int target[2],const unsigned duration[2]);
static void body_stop_axis(int id);
static int torque(int id, bool on)
{
  if(id<1 || id>2)return -EINVAL;
  if(!on){body_stop_axis(id);return body_sync_off(1u<<(id-1));}
  uint8_t v=1;return servo_write(id,40,&v,1);
}

static struct body_ramp g_ramp[2];
static bool body_ramps_active(void){return g_ramp[0].active || g_ramp[1].active;}
static void body_stop_axis(int id){if(id>=1 && id<=2)body_ramp_cancel(&g_ramp[id-1]);}
static void body_ramps_cancel(void){body_ramp_cancel(&g_ramp[0]);body_ramp_cancel(&g_ramp[1]);g_sync_window.active=false;}
static int body_ramps_work(void)
{
 int target[2]={0,0};unsigned duration[2]={0,0},mask=0;
 for(unsigned i=0;i<2;i++)if(body_ramp_next(&g_ramp[i],now_ms(),&target[i],&duration[i]))mask|=1u<<i;
 return mask?body_sync_move(mask,target,duration):0;
}

static void release_servos(void)
{
  body_ramps_cancel();
  if(!g_servo_ok || g_power_state==BODY_FAULTED)return; /* Never retry UART after fault. */
  (void)body_sync_off(3);
}

static bool motor_goal_feedback_ok(int id,int *goal);
static unsigned g_axis_fresh_noops;
static int move_one(int id,int target)
{
  if(id<1 || id>2)return -EINVAL;
  if(!body_servo_target_valid(id,target))return -ERANGE;
  /* Official Servo::move always schedules a final target snap, including
   * equal-angle requests. A fresh goal read alone must not skip WritePos:
   * a torque-OFF servo may track its encoder into the goal register. */
  int goal=-1;bool healthy=motor_goal_feedback_ok(id,&goal);
  if(!healthy && g_feedback_rc[id-1]<0)return g_feedback_rc[id-1];
  if(!healthy)
    {usleep(20000);healthy=motor_goal_feedback_ok(id,&goal);
     if(!healthy)return g_feedback_rc[id-1]<0?g_feedback_rc[id-1]:-ERANGE;}
  int current=g_feedback_position[id-1];
  if(!body_feedback_raw_valid(id,current) || !body_servo_target_valid(id,target))return -ERANGE;
  int anchor=body_feedback_anchor(id,current);
  /* Keep the existing bounded first command even for a valid sensor value. */
  if(abs(anchor-current)>3)return -ERANGE;
  int rc=body_ramp_begin(&g_ramp[id-1],id,anchor,target,now_ms());
  if(rc==0 && anchor!=current)
    {g_ramp[id-1].initial_anchor=true;
     printf("[BodyEncoderAnchor] id=%d measured=%d first_legal_goal=%d max_step=3 command_limits_unchanged=1\n",id,current,anchor);}
  if(rc>=0)
    {if(!g_ramp[id-1].active){g_ramp[id-1].active=true;g_axis_fresh_noops++;
      printf("[BodyOfficialSnap] id=%d target=%d actual=%d fresh_goal=%d position_write_required=1\n",id,target,current,goal);}
     g_moves++;}
  return rc;
}

/* Official limits: abs(load)>=650 or abs(current)>=350, two samples.
 * Missing feedback also stops the action. Cut the motor rail if the serial
 * link is broken: sending a torque-off packet alone cannot stop that case. */
/* Same protected trajectory implementation; a diagnostic can request one
 * axis without silently re-arming the other one. No alternate UART path. */
static int body_start_axes(unsigned axes,const int target[2],int result[2])
{
 if(!target || !result || !axes || (axes&~3u))return -EINVAL;
 result[0]=result[1]=0;
 if(g_power_state!=BODY_READY || !g_servo_ok || !g_motion || g_sleeping)return -EACCES;
 for(unsigned i=0;i<2;i++)if((axes&(1u<<i)) && !body_servo_target_valid(i+1,target[i]))return -ERANGE;
 for(unsigned i=0;i<2;i++)if(axes&(1u<<i))
   {result[i]=move_one(i+1,target[i]);if(result[i]<0)return result[i];}
 return 0;
}

/* Use official-sized reads: exact2-byte goal, then full15-byte health.
 * Avoid the wide block between goal and health, including reserved bytes,
 * in a29-byte request during motion.
 * Both checked replies still share the original frame60/100ms budget;
 * every frame still samples both axes, with original20ms confirmation. */
enum { BODY_GOAL_SNAPSHOT_BYTES=17 };
static bool motor_feedback_accept(int id,const uint8_t feedback[15])
{
  int slot=id-1;g_feedback_rc[slot]=0;
  body_sample_record(id,feedback);
  g_feedback_position[slot]=((unsigned)feedback[0]<<8)|feedback[1];
  if(!body_feedback_raw_valid(id,g_feedback_position[slot])){g_feedback_rc[slot]=-ERANGE;return false;}
  g_pos[slot]=g_feedback_position[slot];
  unsigned l=(((unsigned)feedback[4]<<8)|feedback[5])&0x3ff;
  unsigned c=(((unsigned)feedback[13]<<8)|feedback[14])&0x7fff;
  g_feedback_load[slot]=l;g_feedback_current[slot]=c;
  if(l>g_peak_load[slot])g_peak_load[slot]=l;
  if(c>g_peak_current[slot])g_peak_current[slot]=c;
  return l<650 && c<350;
}
static bool motor_feedback_ok(int id)
{
  uint8_t feedback[15];int rc=servo_read(id,56,feedback,sizeof(feedback));
  g_feedback_rc[id-1]=rc;if(rc<0)return false;
  return motor_feedback_accept(id,feedback);
}
static bool motor_goal_feedback_ok(int id,int *goal)
{
  uint8_t wire_goal[2];
  int rc=servo_read(id,42,wire_goal,sizeof(wire_goal));g_feedback_rc[id-1]=rc;
  if(rc<0)return false;
  *goal=((unsigned)wire_goal[0]<<8)|wire_goal[1];
  return motor_feedback_ok(id);
}

/* An absent ACK is provisional only: fresh exact goal and both health
 * replies classify it inside the ORIGINAL frame window. Never retry unknown
 * state, first-frame old goals, unhealthy feedback or a latched fault. */
static int body_ackprobe_after_absence(unsigned id,int expected,bool absence)
{
 uint32_t frame_started=g_sync_window.started;uint8_t goal[2];int rc=-ENOMSG;
 int goal_rc=-ENODATA,health_rc[2]={-ENODATA,-ENODATA},observed=-1;
 bool first_health_clean=true;unsigned valid=0;if(absence)g_ackprobe_events++;g_ackprobe_id=id;g_ackprobe_expected=expected;
 if(!g_ackprobe_enabled || id<1 || id>2 || g_power_state!=BODY_READY || !g_sync_window.active)
   {rc=-EACCES;goto done;}
 /* Same three fresh READs, different diagnostic order: the other axis
  * first distinguishes a whole-bus outage from the addressed servo. The
  * first hard error still cuts immediately; no post-fault probe is added. */
 unsigned order[2]={id==1?2u:1u,id};
 for(unsigned slot=0;slot<2;slot++)
   {
    unsigned axis=order[slot];
    if(slot==1)
      {
       goal_rc=servo_read(id,42,goal,2);
       if(goal_rc<0){rc=goal_rc;goto done;}
       observed=((unsigned)goal[0]<<8)|goal[1];
       if(!body_servo_target_valid(id,observed)){rc=-ERANGE;goto done;}
      }
    bool healthy=motor_feedback_ok(axis);health_rc[axis-1]=g_feedback_rc[axis-1];
    if(!healthy && health_rc[axis-1]>=0)
      {
        first_health_clean=false;usleep(20000);
        healthy=motor_feedback_ok(axis);health_rc[axis-1]=g_feedback_rc[axis-1];
      }
    if(!healthy){rc=health_rc[axis-1]<0?health_rc[axis-1]:-ERANGE;goto done;}
    valid|=1u<<(axis-1);
   }
 if(observed==expected)rc=0;
 else if(first_health_clean && !g_goal_retry_mask && g_ramp[id-1].step>0 && observed==g_ramp[id-1].current &&
         body_sync_budget(&g_sync_window,now_ms(),60)>20)rc=2;
 else rc=-EIO;
 done:
 g_ackprobe_observed=observed;g_ackprobe_goal_rc=goal_rc;
 g_ackprobe_health_rc0=health_rc[0];g_ackprobe_health_rc1=health_rc[1];
 g_ackprobe_valid_mask=valid;g_ackprobe_applied=goal_rc==0 && observed==expected && valid==3;
 int off=-1;if(rc<0 && g_power_state!=BODY_FAULTED)off=body_transport_fail_stop();
 g_ackprobe_elapsed=now_ms()-frame_started;
 printf("[BodyAckGuard] id=%u expected=%d observed=%d goal_rc=%d health_rc=%d,%d healthy_mask=%u goal_register_proven=%u classification=%d cut_rc=%d elapsed_ms=%u shared_original_window=1 no_commit_until_final_goal_health=1 other_health_first=1\n",id,expected,observed,goal_rc,health_rc[0],health_rc[1],valid,(unsigned)g_ackprobe_applied,rc,off,(unsigned)g_ackprobe_elapsed);
 return rc;
}
#include "body_sync_runtime.inc"
#include "body_official_flow.inc"

static void probe(void)
{
  uint8_t lo = 0, hi = 0, version = 0, again = 0;
  uint8_t touch = 0, bmi = 0;
  bool valid;
  valid = false;
  uint8_t first = g_py_addr;
  for (int attempt = 0; attempt < 2 && !valid; attempt++)
    {
      uint8_t address = attempt == 0 ? first : (first == 0x6f ? 0x71 : 0x6f);
      lo = hi = version = again = 0;
      valid = rd(address, 0x02, &version) >= 0 &&
              rd(address, 0x02, &again) >= 0 && version == again &&
              version != 0 && version != 0xff &&
              rd(address, 0x00, &lo) >= 0 && rd(address, 0x01, &hi) >= 0;
      if (valid) g_py_addr = address;
    }
  if (!valid)
    {
      /* A lost controller invalidates stale servo readiness as well. */
      g_servo_ok = false;
      g_motion = false;
    }
  g_py_ok = valid;
  g_version = version;
  g_uid = lo | (hi << 8);
  rd(0x69, 0, &bmi);
  g_bmi_id = bmi;
  g_touch_ok = valid && bmi == 0x24 && rd(SI12, 0x10, &touch) >= 0;
  g_touch_raw = touch;
  if (valid)
    {
      /* RGB only: preserve motor power bit during passive detection. */
      bit(0x04, 1 << 5, true);
      bit(0x0c, 1 << 5, false);
      bit(0x0a, 1 << 5, true);
      bit(0x14, 1 << 5, false);
      wr(PY32, 0x24, 12);
      leds(0, 12, 4);
    }
  if (g_touch_ok)
    {
      int ret = 0;
      int reg;
      for (reg = 0x0a; reg <= 0x0f; reg++)
        if (wr(SI12, reg, 0) < 0) ret = -EIO;
      if (wr(SI12, 0x09, 0x0f) < 0 || wr(SI12, 0x09, 0x07) < 0 ||
          wr(SI12, 0x08, 0x22) < 0) ret = -EIO;
      for (reg = 2; reg <= 6; reg++)
        if (wr(SI12, reg, 0x33) < 0) ret = -EIO;
      g_touch_ok = ret >= 0;
    }
  printf("[Body] probe py32=%d uid=%04x version=%02x bmi=%02x touch=%d raw=%02x\n",
         g_py_ok, g_uid, g_version, g_bmi_id, g_touch_ok, g_touch_raw);
  printf("[Body] controller_addr=0x%02x\n", g_py_addr);
}

/* Read documented identification/status registers without enabling motors. */
static void body_diagnostic(void)
{
  static const struct { uint8_t address; uint8_t reg; } checks[] =
    {{0x6f,0x02},{0x71,0x02},{0x68,0x10},{0x41,0xfe},{0x50,0x3f},{0x69,0x00}};
  for (unsigned i = 0; i < sizeof(checks)/sizeof(checks[0]); i++)
    {
      uint8_t value = 0;
      int ret = rd(checks[i].address, checks[i].reg, &value);
      printf("[BodyDiag] addr=0x%02x reg=0x%02x rc=%d value=0x%02x\n",
             checks[i].address, checks[i].reg, ret, value);
    }
  printf("[BodyDiag] complete (read-only; missing addresses increase i2c_errors)\n");
}


/* Isolated LED/register timing comparison. No servo power-on, torque or
 * movement. The immediate arm deliberately bypasses the production guard.
 * Never publishes servo readiness. Verify OFF before and after every arm. */
static void body_led_bus_test(void)
{
  uint8_t version=0,rail=0xff;
  int rc=-ENODEV,off_rc=-ENODEV,verify_rc=-ENODEV;
  unsigned failed=0, samples=0;
  static const unsigned gaps[3]={0,1000,10000};
  static const uint8_t regs[4]={0x03,0x0b,0x09,0x05};
  g_motion=false;g_servo_ok=false;
  printf("[BodyLedBus] begin motor_power=off samples=12\n");
  if(rd(PY32,0x02,&version)<0 || version!=0x41)goto done;
  g_py_ok=true;g_version=version;
  if(bit(0x05,1,false)<0 || rd(PY32,0x05,&rail)<0 || (rail&1))goto done;
  if(bit(0x04,32,true)<0 || bit(0x0c,32,false)<0 ||
     bit(0x0a,32,true)<0 || bit(0x14,32,false)<0)goto done;
  if(leds(0,8,2)<0)goto done;
  for(unsigned n=0;n<12;n++)
    {
      unsigned gap=gaps[n%3];
      if(g_feedback_cancel){rc=-ECANCELED;goto done;}
      if(rd(PY32,0x05,&rail)<0 || (rail&1)){rc=-EIO;goto done;}
      if(wr(PY32,0x24,12|0x40)<0){rc=-EIO;goto done;}
      if(gap)usleep(gap);
      unsigned bad=0;int first_reg=-1,first_rc=0;
      for(unsigned i=0;i<4;i++)
        {
          uint8_t value=0;
          int ret=hal_i2c_read_reg(g_bus,PY32,regs[i],&value,BODY_FREQ);
          if(ret<0){bad++;g_i2c_errors++;if(first_reg<0){first_reg=regs[i];first_rc=ret;}}
        }
      samples++;failed+=bad;
      /* Let PY32 finish before OFF verification, never retry a power-on. */
      usleep(20000);
      if(rd(PY32,0x05,&rail)<0 || (rail&1)){rc=-EIO;goto done;}
      printf("[BodyLedBus] sample=%u gap_us=%u reads=4 failures=%u first_reg=%d first_rc=%d rail_off=1\n",n+1,gap,bad,first_reg,first_rc);
    }
  rc=0;
done:
  if(g_py_ok){off_rc=bit(0x05,1,false);verify_rc=rd(PY32,0x05,&rail);}
  bool off=off_rc>=0 && verify_rc>=0 && !(rail&1);
  if(!off)rc=-EIO;
  pthread_mutex_lock(&g_lock);
  g_request&=REQ_LED|REQ_AUTO;g_feedback_test=false;g_feedback_cancel=false;
  g_motion=false;g_servo_ok=false;
  pthread_mutex_unlock(&g_lock);
  printf("[BodyLedBus] done rc=%d samples=%u failures=%u rail_off=%d motion=0 ready=0\n",rc,samples,failed,off);
}

/* Explicit stationary diagnostic, never on the normal boot path.
 * Does not call move_one() or torque(true), and never
 * publishes servo readiness. OFF mode never enables the motor rail.
 * ON mode sends only fixed torque-OFF writes and reads, then cuts power.
 * The 1200ms software budget is checked between bounded operations; it
 * is not an independent hardware cutoff if the CPU or I2C driver stalls.
 */
static int feedback_test_continue(uint32_t start)
{
  if (!g_running || g_feedback_cancel) return -ECANCELED;
  return (uint32_t)(now_ms() - start) > 1200 ? -ETIMEDOUT : 0;
}

static int feedback_test_uart_open(void)
{
  struct termios t;
  if (g_uart >= 0){esp32s3_uart1_resume_pins();return g_txproof_enabled?esp32s3_uart1_txproof_rearm_input():0;}
  g_uart = open("/dev/ttyS1", O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (g_uart < 0) return -errno;
  if (tcgetattr(g_uart, &t) < 0) goto fail;
  cfmakeraw(&t);
  cfsetispeed(&t, B1000000);
  cfsetospeed(&t, B1000000);
  t.c_cflag |= CLOCAL | CREAD;
  t.c_cflag &= ~(CRTSCTS|CSTOPB|PARENB|PARODD|CSIZE);
  t.c_cflag |= CS8;
  if (tcsetattr(g_uart, TCSANOW, &t) < 0) goto fail;
  if(g_txproof_enabled && esp32s3_uart1_txproof_rearm_input()<0)goto fail;
  return 0;
fail:
  close(g_uart);g_uart=-1;return -EIO;
}

/* Only safe register40=0 is accepted. Startup requires the official
 * default ACK boundary before addressing the next servo. Readback remains
 * mandatory afterwards; no target/enable command is accepted here. */
static int feedback_test_torque_off(uint8_t id)
{
  if(id!=1 && id!=2)return -EINVAL;
  if(g_sync_window.active)return -EBUSY;
  uint8_t off=0;
  g_sync_window.started=now_ms();g_sync_window.active=true;
  int rc=servo_write(id,40,&off,1);
  g_sync_window.active=false;
  return rc;
}

static int body_feedback_trace(unsigned phase);
static void body_uart_queue(unsigned phase);

static void body_feedback_test(bool powered)
{
  uint32_t start=now_ms(),rx_before,rx_after,stamp;
  int rc=0,off_rc=-ENODEV,verify_rc=-ENODEV;
  uint8_t rail=0xff;
  unsigned reads=0;
  g_motion=false;g_servo_ok=false;
  esp32s3_uart1_rx_snapshot(&stamp,&rx_before);
  printf("[BodyFeedback] begin powered=%d budget_ms=1200 motion=0 ready=0\n",powered);
  if (!g_py_ok) {rc=-ENODEV;goto done;}
  if ((rc=feedback_test_continue(start))<0) goto done;
  if (bit(0x05,1,false)<0 || rd(PY32,0x05,&rail)<0 || (rail&1))
    {rc=-EIO;goto done;}
  if ((rc=feedback_test_uart_open())<0) goto done;
  body_uart_queue(20);
  if ((rc=body_feedback_trace(10))<0) goto done;
  if (powered)
    {
      static const uint8_t regs[4]={0x03,0x0b,0x09,0x05};
      static const bool values[4]={true,false,true,true};
      for (unsigned i=0;i<4;i++)
        {
          if ((rc=feedback_test_continue(start))<0) goto done;
          if (bit(regs[i],1,values[i])<0) {rc=-EIO;goto done;}
        }
      for (unsigned i=0;i<25;i++)
        {
          if ((rc=feedback_test_continue(start))<0) goto done;
          usleep(20000);
        }
      for (int id=1;id<=2;id++)
        {
          if ((rc=feedback_test_continue(start))<0) goto done;
          if ((rc=feedback_test_torque_off(id))<0) goto done;
          body_uart_queue(30+id);
        }
      if ((rc=body_feedback_trace(11))<0) goto done;
      for (int id=1;id<=2;id++)
        {
          uint8_t enabled=0xff;
          if ((rc=feedback_test_continue(start))<0) goto done;
          body_uart_queue(40+id*2);
          rc=servo_read(id,40,&enabled,1);
          body_uart_queue(41+id*2);
          int trace_rc=body_feedback_trace(12);
          if(rc>=0 && trace_rc<0)rc=trace_rc;
          printf("[BodyFeedback] torque id=%d rc=%d disabled=%d\n",id,rc,rc==0 && enabled==0);
          if (rc<0 || enabled!=0) {if(rc==0)rc=-EIO;goto done;}
        }
    }
  for (unsigned i=0;i<(powered?4u:2u);i++)
    {
      int id=(int)(i%2)+1;
      uint8_t data[15]={0};
      int sample_rc;
      if ((rc=feedback_test_continue(start))<0) goto done;
      uint32_t before=now_ms();
      sample_rc=servo_read(id,56,data,sizeof(data));reads++;
      unsigned position=((unsigned)data[0]<<8)|data[1];
      unsigned load=(((unsigned)data[4]<<8)|data[5])&0x3ff;
      unsigned current=(((unsigned)data[13]<<8)|data[14])&0x7fff;
      printf("[BodyFeedback] sample id=%d rc=%d elapsed_ms=%u valid=%d position=%u load=%u current=%u\n",
             id,sample_rc,(uint32_t)(now_ms()-before),sample_rc==0,position,load,current);
      if (powered && (sample_rc<0 || position>1000 || load>=650 || current>=350))
        {rc=sample_rc<0?sample_rc:-ERANGE;goto done;}
    }
  rc=feedback_test_continue(start);
done:
  /* All exits, including setup/cancel/timeouts, attempt rail OFF and read it
   * back. No diagnostic result makes public motion commands available.
   */
  if (g_py_ok)
    {off_rc=bit(0x05,1,false);verify_rc=rd(PY32,0x05,&rail);}
  bool off_confirmed=off_rc>=0 && verify_rc>=0 && !(rail&1);
  if (!off_confirmed) rc=-EIO;
  if(g_py_ok && body_feedback_trace(13)<0 && rc==0)rc=-EIO;
  body_uart_queue(99);
  if (g_uart>=0) close(g_uart);
  g_uart=-1;
  esp32s3_uart1_rx_snapshot(&stamp,&rx_after);
  pthread_mutex_lock(&g_lock);
  g_request&=REQ_LED|REQ_AUTO;
  g_motion=false;g_servo_ok=false;
  g_feedback_test=false;g_feedback_cancel=false;
  pthread_mutex_unlock(&g_lock);
  printf("[BodyFeedback] done powered=%d rc=%d reads=%u rx_bytes=%u off_rc=%d verify_rc=%d rail_off=%d elapsed_ms=%u motion=0 ready=0\n",
         powered,rc,reads,(uint32_t)(rx_after-rx_before),off_rc,verify_rc,off_confirmed,(uint32_t)(now_ms()-start));
}

extern int esp32s3_uart1_diag_snapshot(uint32_t *values,unsigned count);
extern int esp32s3_uart1_diag_loopback(bool enabled);
extern int esp32s3_uart1_diag_pad_sample(uint32_t *levels);

static int body_uart_hw(unsigned phase,uint32_t *v)
{
  int rc=esp32s3_uart1_diag_snapshot(v,14);
  if(rc<0)return rc;
  printf("[BodyUartHW] phase=%u tx_cfg=%08x rx_cfg=%08x tx_mux=%08x rx_mux=%08x enable=%u levels=%u\n",
         phase,v[0],v[1],v[2],v[3],v[4],v[5]);
  printf("[BodyUartHW] phase=%u conf0=%08x clk=%08x div=%08x status=%08x raw=%08x id=%08x fsm=%08x source_hz=%u\n",
         phase,v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13]);
  uint32_t levels=0;
  rc=esp32s3_uart1_diag_pad_sample(&levels);
  printf("[BodyUartPad] phase=%u rc=%d tx=%d rx=%d restored=%d\n",
         phase,rc,rc==0?(int)(levels&1):-1,rc==0?(int)((levels>>1)&1):-1,rc==0);
  return rc;
}

/* PY32 VM_EN is pin0. These are documented direction/output/input,
 * pull-up/pull-down/drive registers, read only. Input readback observes the
 * control pin; it is still not a direct measurement of the motor supply.
 */
static int body_feedback_trace(unsigned phase)
{
  static const uint8_t regs[6]={0x03,0x05,0x07,0x09,0x0b,0x13};
  uint8_t values[6]={0};
  int rc=0;
  for(unsigned i=0;i<6;i++)
    if((rc=rd(PY32,regs[i],values+i))<0)break;
  printf("[BodyPowerPins] phase=%u rc=%d dir=%02x out=%02x in=%02x pu=%02x pd=%02x drv=%02x\n",
         phase,rc,values[0],values[1],values[2],values[3],values[4],values[5]);
  if(rc<0)return rc;
  uint32_t uart[14];
  return body_uart_hw(phase,uart);
}

/* Internal UART1 loopback only, with the motor rail held OFF. The pattern
 * contains no ff-ff servo header and accepts no user-supplied bytes.
 * Every exit restores the prior loopback bit, closes UART1, checks rail-off
 * and keeps public motion disabled. No loopback on the normal boot path.
 */
static void body_uart_test(void)
{
  static const uint8_t pattern[16]={0x55,0xaa,0,0xff,0x12,0x34,0x56,0x78,
                                  0x87,0x65,0x43,0x21,0xf0,0x0f,0x5a,0xa5};
  uint8_t rx[64],rail=0xff;
  uint32_t values[14],saved_loop=0,start=now_ms(),rx_before,rx_after,stamp;
  int rc=0,restore_rc=0,off_rc=-ENODEV,verify_rc=-ENODEV,used=0;
  bool restore_needed=false,matched=false;
  g_motion=false;g_servo_ok=false;
  esp32s3_uart1_rx_snapshot(&stamp,&rx_before);
  printf("[BodyUartTest] begin motor_power=off budget_ms=1200\n");
  if((rc=body_uart_hw(0,values))<0)goto done;
  if(!g_py_ok){rc=-ENODEV;goto done;}
  if((rc=feedback_test_continue(start))<0)goto done;
  if(bit(0x05,1,false)<0 || rd(PY32,0x05,&rail)<0 || (rail&1))
    {rc=-EIO;goto done;}
  if((rc=feedback_test_uart_open())<0)goto done;
  if((rc=body_uart_hw(1,values))<0)goto done;
  saved_loop=values[6]&(1u<<14);
  restore_needed=true;
  if((rc=esp32s3_uart1_diag_loopback(true))<0)goto done;
  if((rc=body_uart_hw(2,values))<0)goto done;
  if((rc=feedback_test_continue(start))<0)goto done;
  if(tcflush(g_uart,TCIFLUSH)<0){rc=-EIO;goto done;}
  uint32_t read_start=now_ms();
  if(write(g_uart,pattern,sizeof(pattern))!=sizeof(pattern)){rc=-EIO;goto done;}
  while((uint32_t)(now_ms()-read_start)<200 && used<(int)sizeof(rx))
    {
      if((rc=feedback_test_continue(start))<0)goto done;
      struct pollfd pfd={.fd=g_uart,.events=POLLIN};
      int poll_rc=poll(&pfd,1,5);
      if(poll_rc<0 && errno!=EINTR){rc=-EIO;goto done;}
      if(poll_rc>0)
        {
          int n=read(g_uart,rx+used,sizeof(rx)-used);
          if(n>0)used+=n;
          else if(n<0 && errno!=EAGAIN && errno!=EINTR){rc=-EIO;goto done;}
        }
      if((rc=feedback_test_continue(start))<0)goto done;
      if((uint32_t)(now_ms()-read_start)>200){rc=-ETIMEDOUT;goto done;}
      for(int i=0;i+(int)sizeof(pattern)<=used;i++)
        if(!memcmp(rx+i,pattern,sizeof(pattern))){matched=true;break;}
      if(matched)break;
    }
  rc=matched?0:-ETIMEDOUT;
  if(body_uart_hw(3,values)<0 && rc==0)rc=-EIO;
done:
  if(restore_needed)
    {
      restore_rc=esp32s3_uart1_diag_loopback(saved_loop!=0);
      if(restore_rc<0)rc=restore_rc;
      if(body_uart_hw(4,values)<0 || (values[6]&(1u<<14))!=saved_loop)
        {restore_rc=-EIO;rc=-EIO;}
    }
  if(g_uart>=0)close(g_uart);
  g_uart=-1;
  if(g_py_ok){off_rc=bit(0x05,1,false);verify_rc=rd(PY32,0x05,&rail);}
  bool rail_off=off_rc>=0 && verify_rc>=0 && !(rail&1);
  if(!rail_off)rc=-EIO;
  esp32s3_uart1_rx_snapshot(&stamp,&rx_after);
  pthread_mutex_lock(&g_lock);
  g_request&=REQ_LED|REQ_AUTO;g_motion=false;g_servo_ok=false;
  g_feedback_test=false;g_feedback_cancel=false;
  pthread_mutex_unlock(&g_lock);
  printf("[BodyUartTest] done rc=%d matched=%d read_bytes=%d rx_bytes=%u restore_rc=%d saved_loop=%u rail_off=%d elapsed_ms=%u motion=0 ready=0\n",
         rc,matched,used,(uint32_t)(rx_after-rx_before),restore_rc,saved_loop!=0,rail_off,(uint32_t)(now_ms()-start));
}

extern int esp32s3_uart1_diag_pad_wave(uint32_t *v,unsigned count);
static void body_uart_wave(void)
{
  uint8_t rail=0xff;uint32_t values[14],v[8]={0},start=now_ms();
  int rc=0,wave_rc=-ECANCELED,off_rc=-ENODEV,verify_rc=-ENODEV;
  g_motion=false;g_servo_ok=false;
  printf("[BodyUartWave] begin motor_power=off budget_ms=1200\n");
  if(!g_py_ok){rc=-ENODEV;goto done;}
  if((rc=feedback_test_continue(start))<0)goto done;
  if(bit(0x05,1,false)<0 || rd(PY32,0x05,&rail)<0 || (rail&1))
    {rc=-EIO;goto done;}
  if((rc=feedback_test_uart_open())<0)goto done;
  if((rc=body_uart_hw(10,values))<0)goto done;
  if((rc=feedback_test_continue(start))<0)goto done;
  wave_rc=esp32s3_uart1_diag_pad_wave(v,8);rc=wave_rc;
  if(rc==0 && (v[0]<2 || v[1]<2 || v[2]<4 || !v[4] || !v[7]))rc=-EIO;
  if((uint32_t)(now_ms()-start)>1200)rc=-ETIMEDOUT;
done:
  if(g_uart>=0)close(g_uart);g_uart=-1;
  if(g_py_ok){off_rc=bit(0x05,1,false);verify_rc=rd(PY32,0x05,&rail);}
  bool rail_off=off_rc>=0 && verify_rc>=0 && !(rail&1);
  if(!rail_off)rc=-EIO;
  pthread_mutex_lock(&g_lock);
  g_request&=REQ_LED|REQ_AUTO;g_motion=false;g_servo_ok=false;
  g_feedback_test=false;g_feedback_cancel=false;
  pthread_mutex_unlock(&g_lock);
  printf("[BodyUartWave] done rc=%d wave_rc=%d high=%u low=%u edges=%u first=%u last=%u fifo=%u fsm=%u restored=%u rail_off=%d elapsed_ms=%u motion=0 ready=0\n",
         rc,wave_rc,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],rail_off,(uint32_t)(now_ms()-start));
}

extern int esp32s3_uart1_diag_receive(uint8_t id,uint32_t *v,unsigned count);
static void body_uart_receive(bool powered)
{
  uint32_t start=now_ms(),values[14];uint8_t rail=0xff;unsigned samples=0,responses=0;
  int rc=0,off_rc=-ENODEV,verify_rc=-ENODEV;
  g_motion=false;g_servo_ok=false;
  printf("[BodyRxPad] begin powered=%d budget_ms=1200 read_only=1\n",powered);
  if(!g_py_ok){rc=-ENODEV;goto done;}
  if((rc=feedback_test_continue(start))<0)goto done;
  if(bit(0x05,1,false)<0 || rd(PY32,0x05,&rail)<0 || (rail&1)){rc=-EIO;goto done;}
  if((rc=feedback_test_uart_open())<0)goto done;
  if(powered)
    {
      const uint8_t regs[]={3,11,9,5};const bool bits[]={true,false,true,true};
      for(unsigned i=0;i<4;i++)
        {if((rc=feedback_test_continue(start))<0)goto done;
         if(bit(regs[i],1,bits[i])<0){rc=-EIO;goto done;}}
      for(unsigned i=0;i<25;i++)
        {if((rc=feedback_test_continue(start))<0)goto done;usleep(20000);}
    }
  if((rc=body_uart_hw(20,values))<0)goto done;
  for(unsigned id=1;id<=2;id++)
    {
      if((rc=feedback_test_continue(start))<0)goto done;
      if(tcflush(g_uart,TCIFLUSH)<0){rc=-EIO;goto done;}
      uint32_t v[14]={0};rc=esp32s3_uart1_diag_receive(id,v,14);samples++;
      uint8_t bytes[64]={0};unsigned used=0;bool valid=false;unsigned position=0;
      if(rc==0)
        {
          for(unsigned i=0;i<4 && used<sizeof(bytes);i++)
            {if((rc=feedback_test_continue(start))<0)break;
             struct pollfd pfd={.fd=g_uart,.events=POLLIN};int n=poll(&pfd,1,5);
             if(n>0){int got=read(g_uart,bytes+used,sizeof(bytes)-used);if(got>0)used+=got;
               else if(got<0 && errno!=EAGAIN && errno!=EINTR){rc=-EIO;break;}}
             else if(n<0 && errno!=EINTR){rc=-EIO;break;}}
          for(unsigned i=0;i+8<=used;i++)
            if(bytes[i]==255 && bytes[i+1]==255 && bytes[i+2]==id && bytes[i+3]==4 && bytes[i+4]==0)
              {unsigned sum=0;for(unsigned j=2;j<8;j++)sum+=bytes[i+j];
               if((sum&255)==255){valid=true;position=((unsigned)bytes[i+5]<<8)|bytes[i+6];break;}}
        }
      if(valid)responses++;
      printf("[BodyRxPad] sample powered=%d id=%u rc=%d tx_low=%u tx_edges=%u rx_low=%u rx_edges=%u tx_first=%u rx_first=%u tx_last=%u rx_last=%u tx_fifo=%u rx_fifo=%u errors=%u restored=%u read_bytes=%u valid=%d position=%u\n",
             powered,id,rc,v[1],v[2],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],used,valid,position);
      if(rc<0)goto done;
    }
done:
  if(g_py_ok){off_rc=bit(0x05,1,false);verify_rc=rd(PY32,0x05,&rail);}
  bool off=off_rc>=0 && verify_rc>=0 && !(rail&1);if(!off)rc=-EIO;
  if(g_uart>=0)close(g_uart);g_uart=-1;
  pthread_mutex_lock(&g_lock);g_request&=REQ_LED|REQ_AUTO;
  g_motion=false;g_servo_ok=false;g_feedback_test=false;g_feedback_cancel=false;pthread_mutex_unlock(&g_lock);
  printf("[BodyRxPad] done powered=%d rc=%d samples=%u responses=%u rail_off=%d elapsed_ms=%u motion=0 ready=0\n",
         powered,rc,samples,responses,off,(uint32_t)(now_ms()-start));
}

static void body_uart_compare(void)
{
  const bool powered=true;uint32_t start=now_ms(),values[14];uint8_t rail=0xff;unsigned samples=0,responses=0;
  int rc=0,off_rc=-ENODEV,verify_rc=-ENODEV;
  g_motion=false;g_servo_ok=false;
  printf("[BodyRxCompare] begin powered=%d budget_ms=1200 read_only=1\n",powered);
  if(!g_py_ok){rc=-ENODEV;goto done;}
  if((rc=feedback_test_continue(start))<0)goto done;
  if(bit(0x05,1,false)<0 || rd(PY32,0x05,&rail)<0 || (rail&1)){rc=-EIO;goto done;}
  if((rc=feedback_test_uart_open())<0)goto done;
  if(powered)
    {
      const uint8_t regs[]={3,11,9,5};const bool bits[]={true,false,true,true};
      for(unsigned i=0;i<4;i++)
        {if((rc=feedback_test_continue(start))<0)goto done;
         if(bit(regs[i],1,bits[i])<0){rc=-EIO;goto done;}}
      for(unsigned i=0;i<25;i++)
        {if((rc=feedback_test_continue(start))<0)goto done;usleep(20000);}
    }
  if((rc=body_uart_hw(20,values))<0)goto done;
  for(unsigned step=0;step<4;step++)
    {
      unsigned id=step<2?2:1;bool normal=step==1||step==2;
      if((rc=feedback_test_continue(start))<0)goto done;
      if(tcflush(g_uart,TCIFLUSH)<0){rc=-EIO;goto done;}
      uint32_t v[14]={0},request_start=now_ms();uint8_t data[2]={0};
      rc=normal?servo_read(id,56,data,2):esp32s3_uart1_diag_receive(id,v,14);samples++;
      uint8_t bytes[64]={0};unsigned used=0;bool valid=normal&&rc==0;
      unsigned position=valid?((unsigned)data[0]<<8)|data[1]:0;
      if(rc==0 && !normal)
        {
          for(unsigned i=0;i<4 && used<sizeof(bytes);i++)
            {if((rc=feedback_test_continue(start))<0)break;
             struct pollfd pfd={.fd=g_uart,.events=POLLIN};int n=poll(&pfd,1,5);
             if(n>0){int got=read(g_uart,bytes+used,sizeof(bytes)-used);if(got>0)used+=got;
               else if(got<0 && errno!=EAGAIN && errno!=EINTR){rc=-EIO;break;}}
             else if(n<0 && errno!=EINTR){rc=-EIO;break;}}
          for(unsigned i=0;i+8<=used;i++)
            if(bytes[i]==255 && bytes[i+1]==255 && bytes[i+2]==id && bytes[i+3]==4 && bytes[i+4]==0)
              {unsigned sum=0;for(unsigned j=2;j<8;j++)sum+=bytes[i+j];
               if((sum&255)==255){valid=true;position=((unsigned)bytes[i+5]<<8)|bytes[i+6];break;}}
        }
      if(valid)responses++;
      printf("[BodyRxCompare] route step=%u id=%u normal=%d rc=%d valid=%d position=%u request_ms=%u rx_low=%u rx_edges=%u rx_fifo=%u read_bytes=%u restored=%u\n",
             step,id,normal,rc,valid,position,(uint32_t)(now_ms()-request_start),v[4],v[5],v[11],used,v[13]);
      /* A missing/invalid normal reply is an observation; compare both IDs.
       * Direct sampler setup/cancellation errors abort with immediate cleanup. */
      if(normal && (rc==-ETIMEDOUT || rc==-EIO))rc=0;
      if(rc<0)goto done;
    }
done:
  if(g_py_ok){off_rc=bit(0x05,1,false);verify_rc=rd(PY32,0x05,&rail);}
  bool off=off_rc>=0 && verify_rc>=0 && !(rail&1);if(!off)rc=-EIO;
  if(g_uart>=0)close(g_uart);g_uart=-1;
  pthread_mutex_lock(&g_lock);g_request&=REQ_LED|REQ_AUTO;
  g_motion=false;g_servo_ok=false;g_feedback_test=false;g_feedback_cancel=false;pthread_mutex_unlock(&g_lock);
  printf("[BodyRxCompare] done powered=%d rc=%d samples=%u responses=%u rail_off=%d elapsed_ms=%u motion=0 ready=0\n",
         powered,rc,samples,responses,off,(uint32_t)(now_ms()-start));
}

extern int esp32s3_uart1_diag_queue(uint32_t *v,unsigned count);
extern uint32_t esp32s3_uart1_rx_error_begin(void);
static void body_uart_queue(unsigned phase)
{
  uint32_t v[10]={0};int rc=esp32s3_uart1_diag_queue(v,10);
  printf("[BodyQueue] phase=%u rc=%d tx_head=%u tx_tail=%u rx_head=%u rx_tail=%u conf1=%08x ena=%08x st=%08x raw=%08x status=%08x fsm=%08x\n",
         phase,rc,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]);
}

extern int board_cores3_pmic_read_reg(uint8_t reg,uint8_t *value);
static int body_power_mv(uint8_t reg)
{
  uint8_t h0=0,h1=0,lo=0;
  int a=board_cores3_pmic_read_reg(reg,&h0);
  int b=board_cores3_pmic_read_reg(reg+1,&lo);
  int c=board_cores3_pmic_read_reg(reg,&h1);
  if(a<0 || b<0 || c<0)return -EIO;
  if(h0!=h1)return -EAGAIN; /* Do not splice ADC values across a carry. */
  return ((int)h0<<8)|lo;
}
static void body_power_trace(uint32_t now)
{
  printf("[BodyPower] ms=%u vbus_mv=%d vbat_mv=%d motion=%d settled=%d focus=%d hold=%d\n",
         (unsigned)now,body_power_mv(0x38),body_power_mv(0x34),
         g_motion,g_settled,g_focused,g_focus_hold);
}


/* Only the body worker touches I2C/UART. Waking the screen never enables
 * motors. An explicit action may prepare once; a fault is never auto-cleared.
 * Startup OFF writes require ACKs; both torque registers and full feedback
 * must also be read successfully before publishing readiness. */
static int prepare_continue(uint32_t start)
{
  if(!g_running || g_sleeping || g_prepare_cancel)return -ECANCELED;
  return (uint32_t)(now_ms()-start)>=BODY_PREPARE_BUDGET_MS?-ETIMEDOUT:0;
}
static void body_prepare_worker(void)
{
  stackchan_body_official_reset();
  uint32_t started=now_ms(),stamp,rx_before,rx_after;
  uint8_t rail=0xff;
  int rc=0,off_rc=-ENODEV,verify_rc=-ENODEV;
  int positions[2]={-1,-1};unsigned stage=0;
  uint32_t phase_ms[5]={0};unsigned settle_naps=0;
  esp32s3_uart1_rx_snapshot(&stamp,&rx_before);
  g_motion=false;g_servo_ok=false;
  if(!body_power_may_prepare(g_power_state)){rc=-EACCES;goto done;}
  g_power_state=BODY_PREPARING;
  if((rc=prepare_continue(started))<0)goto done;
  stage=1;
  if(!g_py_ok)probe();
  if(!g_py_ok){rc=-ENODEV;goto done;}
  if(bit(0x05,1,false)<0 || rd(PY32,0x05,&rail)<0 || (rail&1))
    {rc=-EIO;goto done;}
  stage=2;
  /* Same PY32 GPIO semantics as official setDirection/setPullMode/digitalWrite. */
  if(bit(0x03,1,true)<0 || bit(0x0b,1,false)<0 ||
     bit(0x09,1,true)<0 || bit(0x05,1,true)<0){rc=-EIO;goto done;}
  g_servo_reply_level[0]=g_servo_reply_level[1]=-1;
  stage=3;
  /* Measured cold motor readiness is later than the controller's readiness.
   * Wait before the FIRST servo command; do not retry a failed handshake. */
  phase_ms[0]=now_ms()-started;
  {uint32_t settle_started=now_ms();unsigned nap;
   while((nap=body_prepare_settle_left(settle_started,now_ms()))!=0)
     {if((rc=prepare_continue(started))<0)goto done;settle_naps++;usleep(nap*1000);}}
  phase_ms[1]=now_ms()-started;
  /* Official lifecycle: VM enabled and settled before connecting UART. */
  if((rc=feedback_test_uart_open())<0)goto done;
  for(int id=1;id<=2;id++)
    {if((rc=prepare_continue(started))<0)goto done;
     if((rc=feedback_test_torque_off(id))<0)goto done;}
  phase_ms[2]=now_ms()-started;usleep(2000);
  phase_ms[3]=now_ms()-started;stage=4;
  for(int id=1;id<=2;id++)
    {
      uint8_t enabled=0xff,table[13];struct body_servo_config cfg;
      if((rc=prepare_continue(started))<0)goto done;
      if((rc=servo_read(id,0,table,sizeof(table)))<0)goto done;
      bool valid=body_servo_config_decode(id,table,&cfg);
      printf("[BodyServoConfig] id=%d table_id=%u valid=%d endian=%u baud=%u reply_level=%u min=%u max=%u firmware=%u.%u model=%u.%u readonly=1\n",id,cfg.id,valid,cfg.endian,cfg.baud,cfg.reply_level,cfg.min,cfg.max,cfg.fw_major,cfg.fw_minor,cfg.model_major,cfg.model_minor);
      if(!valid){rc=-EPROTO;goto done;}
      g_servo_reply_level[id-1]=cfg.reply_level;g_servo_min[id-1]=cfg.min;g_servo_max[id-1]=cfg.max;
      if((rc=prepare_continue(started))<0)goto done;
      if((rc=servo_read(id,40,&enabled,1))<0)goto done;
      if(enabled!=0){rc=-EIO;goto done;}
      if((rc=prepare_continue(started))<0)goto done;
      int snapshot_goal=-1;
      if(!motor_goal_feedback_ok(id,&snapshot_goal))
        {rc=g_feedback_rc[id-1]<0?g_feedback_rc[id-1]:-ERANGE;goto done;}
      unsigned pos=g_feedback_position[id-1],load=g_feedback_load[id-1],current=g_feedback_current[id-1];
      printf("[BodyGoalSnapshot] startup id=%d goal=%d health_bytes=15 snapshot_bytes=17 checked_responses=2 motion=0\n",id,snapshot_goal);
      printf("[BodyPrepare] feedback id=%d pos=%u load=%u current=%u torque_off=1\n",id,pos,load,current);
      if(!body_feedback_raw_valid(id,pos) || load>=650 || current>=350){rc=-ERANGE;goto done;}
      positions[id-1]=pos;
    }
  if((rc=prepare_continue(started))<0)goto done;
  if(rc>=0 && atomic_exchange(&g_observe_armed,false))
    {
      rc=body_observe_arm(true,esp32s3_uart1_timing_config,esp32s3_uart1_wire_counter_config);
      printf("[BodyObserve] activated rc=%d power_or_motion_requested=0 one_shot=1\n",rc);
    }
done:
  if(rc<0)
    {
      /* Cancel and every electrical failure leave the motor rail OFF. */
      if(g_bus){off_rc=bit(0x05,1,false);verify_rc=rd(PY32,0x05,&rail);}
      if(off_rc<0 || verify_rc<0 || (rail&1))rc=-EIO;
      if(g_uart>=0)close(g_uart);g_uart=-1;
      g_power_state=body_power_after_prepare(g_power_state,rc);
      g_servo_ok=false;g_motion=false;
    }
  else
    {g_pos[0]=positions[0];g_pos[1]=positions[1];g_settled=true;
     g_power_state=BODY_READY;g_servo_ok=true;}
  esp32s3_uart1_rx_snapshot(&stamp,&rx_after);
  phase_ms[4]=now_ms()-started;
  printf("[BodyPrepareTiming] before_settle=%u after_settle=%u after_torque_OFF=%u before_reads=%u finished=%u naps=%u budget_ms=%u elapsed_settle=1\n",phase_ms[0],phase_ms[1],phase_ms[2],phase_ms[3],phase_ms[4],settle_naps,BODY_PREPARE_BUDGET_MS);
  printf("[BodyPrepare] done rc=%d stage=%u state=%d ready=%d yaw=%d pitch=%d rx_bytes=%u elapsed_ms=%u motion=0\n",
         rc,stage,g_power_state,g_servo_ok,g_pos[0],g_pos[1],(uint32_t)(rx_after-rx_before),(uint32_t)(now_ms()-started));
  if(rc>=0){uint32_t clock_diag[14];if(esp32s3_uart1_diag_snapshot(clock_diag,14)==0)printf("[BodyClock] source_hz=%u clock=%08x divider=%08x\n",clock_diag[13],clock_diag[7],clock_diag[8]);}
  g_prepare_rc=rc;g_prepare_done=true;g_prepare_pending=false;
}

/* Explicit hypothesis test, not a recovery: compare the VM_EN input level
 * with output high in the original open-drain mode, then push-pull mode.
 * Never enables torque, moves, clears a fault or publishes readiness.
 * INA226 bus voltage is the BAT+ rail, NOT the 5V motor rail. */

#include "body_buscheck.inc"
#include "body_readstress.inc"
#include "body_holdcheck.inc"
static bool g_buscheck_staged,g_buscheck_readonly;
static unsigned g_readstress_gap_us,g_holdcheck_axis;
static void body_buscheck_worker(void)
{
  unsigned completed=0;g_motion=false;
  int rc=g_holdcheck_axis?body_holdcheck_core(128,g_holdcheck_axis,&completed):g_buscheck_readonly?body_readstress_core(128,g_readstress_gap_us,&completed):body_buscheck_core(128,&completed,g_buscheck_staged);g_sync_window.active=false;
  uint8_t rail=0xff;int off=bit(0x05,1,false),verify=rd(PY32,0x05,&rail);
  bool stopped=off>=0 && verify>=0 && !(rail&1);
  if(!stopped){rc=-EIO;g_power_state=BODY_FAULTED;}
  else g_power_state=body_power_after_idle(g_power_state);
  if(g_uart>=0)close(g_uart);g_uart=-1;
  pthread_mutex_lock(&g_lock);g_motion=false;g_servo_ok=false;g_feedback_test=false;g_feedback_cancel=false;pthread_mutex_unlock(&g_lock);
  if(g_buscheck_readonly)printf("[BodyReadStress] done rc=%d cycles=%u expected=128 gap_us=%u torque_on=0 goal_writes=0 rail_off=%d no_retry=1\n",rc,completed,g_readstress_gap_us,stopped);
  if(g_holdcheck_axis)printf("[BodyHoldCheck] done rc=%d cycles=%u expected=128 axis=%u target=%d requested=%u verified=%u max_deviation=%u torque_after_first=%d new_travel=0 rail_off=%d no_fault_retry=1\n",rc,completed,g_holdcheck_axis,g_hold_target,g_hold_requested,g_hold_verified,g_hold_max_deviation,g_hold_torque,stopped);
  else
  printf("[BodyBusCheck] done rc=%d cycles=%u expected=128 staged=%d torque_on=0 executed_position_writes=0 action_commands=0 rail_off=%d no_retry=1\n",rc,completed,g_buscheck_staged,stopped);
}

static void body_power_path_test(void)
{
  uint32_t started=now_ms();uint8_t saved_drive=0xff,rail=0xff;
  int rc=0,off_rc=-EIO,restore_rc=-EIO;bool saved=false;
  g_motion=false;g_servo_ok=false;
  printf("[BodyPowerPath] begin budget_ms=1200 motion=0 ready=0\n");
  if(!g_py_ok){rc=-ENODEV;goto done;}
  if(bit(0x05,1,false)<0 || rd(PY32,0x13,&saved_drive)<0){rc=-EIO;goto done;}
  saved=true;
  {uint8_t reg=2,data[2]={0};struct i2c_msg_s msg[2]={0};
   msg[0].frequency=msg[1].frequency=BODY_FREQ;msg[0].addr=msg[1].addr=0x41;
   msg[0].buffer=&reg;msg[0].length=1;msg[1].flags=I2C_M_READ;
   msg[1].buffer=data;msg[1].length=2;
   int voltage_rc=I2C_TRANSFER(g_bus,msg,2);
   unsigned raw=((unsigned)data[0]<<8)|data[1];
   printf("[BodyPowerPath] BAT_bus_rc=%d raw=%u millivolts=%u motor_voltage_measured=0\n",voltage_rc,raw,raw*5/4);}
  if((rc=feedback_test_uart_open())<0)goto done;
  if(bit(0x03,1,true)<0 || bit(0x0b,1,false)<0 || bit(0x09,1,true)<0 ||
     bit(0x13,1,true)<0 || bit(0x05,1,true)<0){rc=-EIO;goto done;}
  for(unsigned phase=0;phase<2;phase++)
    {
      if((rc=feedback_test_continue(started))<0)goto done;
      if(phase && bit(0x13,1,false)<0){rc=-EIO;goto done;}
      for(unsigned n=0;n<10;n++)
        {if((rc=feedback_test_continue(started))<0)goto done;usleep(20000);}
      if((rc=body_feedback_trace(20+phase))<0)goto done;
      for(int id=1;id<=2;id++)
        {if((rc=feedback_test_continue(started))<0)goto done;
         uint8_t data[2]={0};int read_rc=servo_read(id,56,data,2);
         printf("[BodyPowerPath] phase=%u id=%d read_rc=%d position=%d\n",phase,id,read_rc,read_rc<0?-1:((int)data[0]<<8)|data[1]);}
    }
done:
  if(g_bus){off_rc=bit(0x05,1,false);if(saved)restore_rc=wr(PY32,0x13,saved_drive);
    if(rd(PY32,0x05,&rail)<0)off_rc=-EIO;}
  if(g_uart>=0)close(g_uart);g_uart=-1;
  /* A diagnostic result cannot recover the failed operational state. */
  g_power_state=BODY_FAULTED;
  pthread_mutex_lock(&g_lock);g_request&=REQ_LED|REQ_AUTO;
  g_feedback_test=false;g_feedback_cancel=false;pthread_mutex_unlock(&g_lock);
  printf("[BodyPowerPath] done rc=%d off_rc=%d restore_rc=%d rail_off=%d elapsed_ms=%u motion=0 ready=0 fault_latched=1\n",rc,off_rc,restore_rc,off_rc>=0 && !(rail&1),(uint32_t)(now_ms()-started));
}

/* One explicit stationary observation, never an operational retry. */
static int startup_continue(uint32_t start)
{
  if(!g_running || g_feedback_cancel || g_sleeping)return -ECANCELED;
  return (uint32_t)(now_ms()-start)>=3000?-ETIMEDOUT:0;
}
static void body_startup_sample(unsigned phase,uint32_t powered)
{
  for(int id=1;id<=2;id++)
    {
      uint8_t data[2]={0};uint32_t stamp,before,after;
      esp32s3_uart1_rx_snapshot(&stamp,&before);
      int rc=servo_read(id,56,data,2);
      esp32s3_uart1_rx_snapshot(&stamp,&after);
      printf("[BodyStartup] phase=%u at_ms=%u id=%d rc=%d position=%d rx_bytes=%u\n",phase,(uint32_t)(now_ms()-powered),id,rc,rc<0?-1:((int)data[0]<<8)|data[1],(uint32_t)(after-before));
    }
}
static void body_startup_test(void)
{
  uint32_t started=now_ms(),powered=started;uint8_t rail=0xff;
  int rc=0,off_rc=-EIO,verify_rc=-EIO;
  static const unsigned at_ms[]={200,600,1000,1400,1800,2200};
  g_motion=false;g_servo_ok=false;
  printf("[BodyStartup] begin budget_ms=3000 explicit=1 torque_on=0 motion=0 ready=0\n");
  if((rc=startup_continue(started))<0)goto done;
  if(!g_py_ok){rc=-ENODEV;goto done;}
  if(bit(0x05,1,false)<0 || rd(PY32,0x05,&rail)<0 || (rail&1)){rc=-EIO;goto done;}
  if((rc=feedback_test_uart_open())<0)goto done;
  if(bit(0x03,1,true)<0 || bit(0x0b,1,false)<0 || bit(0x09,1,true)<0 || bit(0x05,1,true)<0){rc=-EIO;goto done;}
  powered=now_ms();
  for(unsigned n=0;n<sizeof(at_ms)/sizeof(at_ms[0]);n++)
    {
      while((uint32_t)(now_ms()-powered)<at_ms[n])
        {if((rc=startup_continue(started))<0)goto done;usleep(10000);}
      if((rc=startup_continue(started))<0)goto done;
      body_startup_sample(n,powered);
      if((rc=body_feedback_trace(30+n))<0)goto done;
    }
  /* Compare a fresh UART open immediately after the final timed baseline,
   * in the SAME power window. No additional power-on or torque operation. */
  if((rc=startup_continue(started))<0)goto done;
  close(g_uart);g_uart=-1;
  if((rc=feedback_test_uart_open())<0)goto done;
  body_startup_sample(6,powered);
done:
  if(g_bus){off_rc=bit(0x05,1,false);verify_rc=rd(PY32,0x05,&rail);}
  if(g_uart>=0)close(g_uart);g_uart=-1;
  g_power_state=BODY_FAULTED;g_motion=false;g_servo_ok=false;
  pthread_mutex_lock(&g_lock);g_request&=REQ_LED|REQ_AUTO;
  g_feedback_test=false;g_feedback_cancel=false;pthread_mutex_unlock(&g_lock);
  printf("[BodyStartup] done rc=%d off_rc=%d verify_rc=%d rail_off=%d elapsed_ms=%u motion=0 ready=0 fault_latched=1\n",rc,off_rc,verify_rc,off_rc>=0 && verify_rc>=0 && !(rail&1),(uint32_t)(now_ms()-started));
}

/* A servo frame has a shared60ms arrival/100ms parse budget. The
 * default priority100 shares a200ms RR slice with UI/network work. Serve
 * bounded body work above those tasks and below audio playback110; IRQs,
 * wire deadlines and fault protections remain unchanged. */
#define BODY_SCHED_PRIORITY 105
static int body_thread_attributes(pthread_attr_t *attr)
{
  struct sched_param param={.sched_priority=BODY_SCHED_PRIORITY};
  int rc=pthread_attr_init(attr);if(rc)return rc;
  if(!(rc=pthread_attr_setdetachstate(attr,PTHREAD_CREATE_DETACHED)) &&
     !(rc=pthread_attr_setstacksize(attr,3072)) &&
     !(rc=pthread_attr_setinheritsched(attr,PTHREAD_EXPLICIT_SCHED)) &&
     !(rc=pthread_attr_setschedpolicy(attr,SCHED_RR)))
    rc=pthread_attr_setschedparam(attr,&param);
  if(rc)pthread_attr_destroy(attr);
  return rc;
}

static void *body_worker(void *arg)
{
  bool touched = false;
  bool armed = false;
  int last_state = -1;
  int nod_stage = 0;
  int nod_home = -1;
  int settle = 0;
  uint32_t nod_at = 0;
  uint32_t release_at = 0;
  uint32_t last_led = 0;
  uint32_t last_press = 0;
  uint32_t last_motor_check = 0;
  uint32_t last_power_trace = 0;
  unsigned touch_errors = 0;
  unsigned motor_bad[2] = {0, 0};
  int move_target[2]={BODY_HOME_YAW,BODY_HOME_PITCH};
  unsigned move_axes=3;
  (void)arg;
  g_bus = esp32s3_i2cbus_initialize(0);
  if(g_bus)
    {
      for(unsigned n=0;n<6;n++){usleep(200000);probe();if(g_py_ok)break;}
      uint8_t rail=0xff;
      int off_rc=bit(0x05,1,false),read_rc=rd(PY32,0x05,&rail);
      g_motion=false;g_servo_ok=false;
      g_power_state=off_rc>=0 && read_rc>=0 && !(rail&1)?BODY_OFF_IDLE:BODY_FAULTED;
      printf("[Body] boot controller=%d state=%d rail_off=%d explicit_prepare=1 motion=0\n",g_py_ok,g_power_state,g_power_state==BODY_OFF_IDLE);
    }
  int body_policy=-1;struct sched_param body_sched={0};
  int body_sched_rc=pthread_getschedparam(pthread_self(),&body_policy,&body_sched);
  printf("[BodySched] requested=%d actual=%d policy=%d rc=%d\n",BODY_SCHED_PRIORITY,body_sched.sched_priority,body_policy,body_sched_rc);
  if(body_sched_rc || body_policy!=SCHED_RR || body_sched.sched_priority!=BODY_SCHED_PRIORITY)
    {g_running=false;g_servo_ok=false;g_motion=false;g_power_state=BODY_FAULTED;}
  while (g_running && g_bus)
    {
      struct timespec loop_deadline;
      bool loop_clock_valid=body_loop_deadline(&loop_deadline);
      unsigned req;
      uint8_t color[3];
      int yaw, pitch, diag_axis, state = 0;
      uint32_t now = now_ms();
      pthread_mutex_lock(&g_lock);
      req = g_request; g_request = 0;
      memcpy(color, g_rgb, sizeof(color));
      yaw = g_target_yaw; pitch = g_target_pitch;diag_axis=g_diag_axis;
      pthread_mutex_unlock(&g_lock);
#ifdef CONFIG_PLANT_AI_VOICE
      state = voice_service_get_state();
#endif
      g_voice = state;
      if(g_power_trace && now-last_power_trace>=100)
        {last_power_trace=now;body_power_trace(now);}
      if(req & REQ_IDLE_POWER)
        {
          body_ramps_cancel();
          g_motion=false;g_servo_ok=false;nod_stage=0;release_at=0;
          g_power_state=body_power_after_idle(g_power_state);
          uint8_t rail=0xff;
          int off=bit(0x05,1,false),verify=rd(PY32,0x05,&rail);
          g_idle_rc=off<0?off:verify<0?verify:(rail&1)?-EIO:0;
          /* RGB-off is optional if a missing controller was detected; the
           * motor OFF readback is mandatory before accepting standby. */
          if(g_idle_rc==0&&g_py_ok)leds(0,0,0);
          printf("[Body] idle_power rc=%d rail_off=%d motion=0\n",g_idle_rc,g_idle_rc==0);
          if(g_idle_rc<0)g_power_state=BODY_FAULTED;
          if(g_uart>=0)close(g_uart);g_uart=-1;
          if(g_prepare_pending){g_prepare_rc=-ECANCELED;g_prepare_done=true;g_prepare_pending=false;}
          g_idle_done=true;req=0;
        }
      if(req & REQ_PREPARE){body_prepare_worker();req&=~REQ_PREPARE;}
      if(req & REQ_BUS_CHECK){nod_stage=0;release_at=0;body_buscheck_worker();req&=REQ_LED|REQ_AUTO;}
      if(req & REQ_STARTUP)
        {nod_stage=0;release_at=0;body_startup_test();req&=REQ_LED|REQ_AUTO;}
      if(req & REQ_POWER_PATH)
        {nod_stage=0;release_at=0;body_power_path_test();req&=REQ_LED|REQ_AUTO;}
      if(req & REQ_LED_BUS)
        {nod_stage=0;release_at=0;body_led_bus_test();req&=REQ_LED|REQ_AUTO;}
      if(req & REQ_UART_COMPARE)
        {nod_stage=0;release_at=0;body_uart_compare();req&=REQ_LED|REQ_AUTO;}
      if(req & REQ_UART_RECEIVE)
        {nod_stage=0;release_at=0;body_uart_receive(g_feedback_powered);req&=REQ_LED|REQ_AUTO;}
      if(req & REQ_UART_WAVE)
        {
          nod_stage=0;release_at=0;body_uart_wave();req&=REQ_LED|REQ_AUTO;
        }
      if(req & REQ_UART_TEST)
        {
          nod_stage=0;release_at=0;
          body_uart_test();
          req&=REQ_LED|REQ_AUTO;
        }
      if(req & REQ_FEEDBACK_TEST)
        {
          nod_stage=0;release_at=0;
          body_feedback_test(g_feedback_powered);
          req&=REQ_LED|REQ_AUTO;
        }
      if(req & REQ_POSE)
        {int a=position(1),b=position(2);
         if(a>=0 && b>=0){g_pos[0]=a;g_pos[1]=b;}
         printf("[Body] pose feedback yaw=%d pitch=%d\n",a,b);}
      if (req & REQ_DIAG) body_diagnostic();
      if (req & REQ_AUTO) last_state = -1;
      if (req & REQ_RELEASE)release_servos();
      if (req & REQ_PROBE)
        {
          /* Re-detect the controller after a boot-time miss or reconnect.
           * Leave motion disabled until an explicit motion command. */
          if(!g_prepare_pending && g_power_state==BODY_OFF_IDLE)
            {g_prepare_cancel=false;body_prepare_worker();}
        }
      if (req & REQ_STOP)
        {
          g_motion = false;
          nod_stage = 0;
          release_at = 0;
          release_servos();
          if(g_servo_ok && g_uart>=0){int a=position(1),b=position(2);
            if(a>=0 && b>=0){g_pos[0]=a;g_pos[1]=b;}}
          g_settled=g_power_state!=BODY_FAULTED;
        }
      if (req & REQ_POWER_OFF)
        {
          g_motion = false;
          g_servo_ok = false;
          g_power_state=body_power_after_idle(g_power_state);
          nod_stage = 0;
          release_at = 0;
          release_servos();
          int off=bit(0x05,1,false);uint8_t rail=0xff;int verify=rd(PY32,0x05,&rail);
          bool confirmed=off>=0 && verify>=0 && !(rail&1);
          if(!confirmed){if(g_power_state!=BODY_FAULTED)g_motor_faults++;g_power_state=BODY_FAULTED;}
          printf("[Body] motor rail off verified=%d\n",confirmed);
          printf("[BodyPowerOff] cut_rc=%d verify_rc=%d rail_off=%d motion=0\n",off,verify,confirmed);
        }
      if (!g_sleeping && (req & REQ_LED)) leds(color[0], color[1], color[2]);
      if ((req & REQ_MOVE) && g_motion && g_servo_ok)
        {
          g_settled = false;
          body_ramps_cancel();
          nod_stage = 0; /* Explicit positioning supersedes a speech nod. */
          memset(g_peak_load,0,sizeof(g_peak_load));memset(g_peak_current,0,sizeof(g_peak_current));
          move_target[0]=g_home[0]+yaw*16/5;
          move_target[1]=g_home[1]+pitch*16/5;
          unsigned axes=(req&REQ_AXIS)?(diag_axis==1?1u:diag_axis==2?2u:0u):3u;
          if(req&REQ_AXIS)
            {for(unsigned i=0;i<2;i++)if(!(axes&(1u<<i)))move_target[i]=g_pos[i];
             printf("[BodyAxisPlan] mask=%u yaw_target=%d pitch_target=%d unrequested_axis_commands=0\n",axes,move_target[0],move_target[1]);}
          move_axes=axes;
          int result[2]={0,0};int dispatch=body_start_axes(axes,move_target,result);
          int a=dispatch<0?dispatch:result[0],b=dispatch<0?dispatch:result[1];
          printf("[Body] move yaw=%d pitch=%d rc=%d,%d\n", yaw, pitch, a, b);
          if(a<0 || b<0){body_ramps_cancel();bit(0x05,1,false);
            g_power_state=BODY_FAULTED;g_servo_ok=false;g_motion=false;release_at=0;}
          else release_at = now_ms() + 650;
        }
      if (((req & REQ_NOD) || (!g_focused && state == 3 && last_state != 3)) &&
          g_motion && g_servo_ok && !nod_stage)
        {
          nod_home = position(2);
          if (nod_home >= 0 && abs(nod_home - g_home[1]) <= 20 &&
              move_one(2, nod_home + 16) >= 0)
            { nod_stage = 1; nod_at = now + 850; release_at = 0; }
        }
      if (nod_stage && (int32_t)(now - nod_at) >= 0)
        {
          if (nod_stage == 1 && move_one(2, nod_home) >= 0)
            { nod_stage = 2; nod_at = now + 850; }
          else
            { torque(2, false); nod_stage = 0; }
        }
      if(g_motion && g_servo_ok && !g_sleeping && body_ramps_active())
        {
          unsigned before_commit=g_sync_committed_frames;
          int rc=body_ramps_work();
          /* Official normal flow checks the configured pitch axis before goals when50ms
           * feedback is due. Never add immediate post-goal feedback bursts.
           * Failed/no-op work cannot postpone the existing100ms watchdog. */
          if(rc==0 && g_sync_committed_frames!=before_commit)
            {last_motor_check=g_sync_window.started;motor_bad[0]=motor_bad[1]=0;}
          if(rc<0)
            {if(g_power_state!=BODY_FAULTED)g_motor_faults++;g_motion=false;g_servo_ok=false;g_power_state=BODY_FAULTED;
             nod_stage=0;release_at=0;body_ramps_cancel();
             int off=bit(0x05,1,false);uint8_t rail=0xff;int verify=rd(PY32,0x05,&rail);
             printf("[Body] ramp write fault rc=%d rail_off=%d no_retry=1\n",rc,off>=0 && verify>=0 && !(rail&1));
             body_wire_fault_summary();body_samples_dump();body_bus_fault_trace();}
          else if(!body_ramps_active() && release_at)release_at=now_ms()+650;
        }
      if (release_at && !body_ramps_active() && (int32_t)(now - release_at) >= 0)
        {
          release_at = 0;
          /* Prove the commanded goal BEFORE torque-OFF can track encoder
           * position into that register. Then verify actual post-OFF pose. */
          int final_rc=stackchan_body_official_final(move_axes,move_target);
          if(final_rc==0 && (!g_focused || !g_focus_hold))
            final_rc=stackchan_body_official_unload(move_target);
          g_settled=final_rc==0 && g_power_state==BODY_READY && g_servo_ok;
          if(!g_settled)
            {
              if(g_power_state!=BODY_FAULTED)g_motor_faults++;g_motion=false;g_servo_ok=false;g_power_state=BODY_FAULTED;
              body_ramps_cancel();bit(0x05,1,false);
              printf("[Body] move did not settle; motor rail off\n");
            }
          printf("[BodyFinal] rc=%d phase=%d id=%d axes=%d goal=%d,%d target=%d,%d pos=%d,%d entry_state=%d entry_servo=%d\n",final_rc,g_official_final_trace[10],g_official_final_trace[11],g_official_final_trace[2],g_official_final_trace[5],g_official_final_trace[6],g_official_final_trace[3],g_official_final_trace[4],g_official_final_trace[7],g_official_final_trace[8],g_official_final_trace[0],g_official_final_trace[1]);
          printf("[Body] settled yaw=%d pitch=%d torque=%s\n",g_pos[0],g_pos[1],g_focused && g_focus_hold?"hold":"off");
          printf("[Body] peak load=%u,%u current=%u,%u\n",g_peak_load[0],g_peak_load[1],g_peak_current[0],g_peak_current[1]);
        }
      uint32_t motor_check_now=now_ms();
      if (g_motion && (nod_stage || release_at || (g_focused && g_focus_hold)) && motor_check_now - last_motor_check >= 100)
        {
          int id;
          last_motor_check = motor_check_now;
          for (id = nod_stage ? 2 : 1; id <= 2; id++)
            {
              if(!body_official_feedback_axis(id,move_axes,nod_stage!=0,g_focused&&g_focus_hold))continue;
              motor_bad[id - 1] = motor_feedback_ok(id) ? 0 : motor_bad[id - 1] + 1;
              if (g_feedback_rc[id-1]<0 || motor_bad[id - 1] >= 2)
                {
                  uint32_t failure_hw[14];
                  int hw_rc=esp32s3_uart1_diag_snapshot(failure_hw,14);
                  if(g_power_state!=BODY_FAULTED)g_motor_faults++;g_motion=false;g_servo_ok=false;g_power_state=BODY_FAULTED;
                  nod_stage=0;release_at=0;body_ramps_cancel();
                  int cut_rc=bit(0x05,1,false);
                  uint8_t cut_rail=0xff;int cut_verify=rd(PY32,0x05,&cut_rail);
                  printf("[BodyFaultStop] cut_rc=%d verify_rc=%d rail_off=%d no_uart_retry_after_fault=1\n",cut_rc,cut_verify,cut_rc>=0 && cut_verify>=0 && !(cut_rail&1));
                  if(hw_rc==0)printf("[BodyFaultHW] tx_cfg=%08x rx_cfg=%08x tx_mux=%08x rx_mux=%08x enable=%u levels=%u conf0=%08x clk=%08x div=%08x status=%08x raw=%08x id=%08x fsm=%08x source_hz=%u\n",failure_hw[0],failure_hw[1],failure_hw[2],failure_hw[3],failure_hw[4],failure_hw[5],failure_hw[6],failure_hw[7],failure_hw[8],failure_hw[9],failure_hw[10],failure_hw[11],failure_hw[12],failure_hw[13]);
                  printf("[Body] fault feedback id=%d rc=%d load=%u current=%u values_valid=%d failures=%u\n",
                         id,g_feedback_rc[id-1],g_feedback_load[id-1],
                         g_feedback_current[id-1],g_feedback_rc[id-1]==0,motor_bad[id-1]);
                  printf("[BodyUartFault] reg=%d rx_used=%d tx_head=%u tx_tail=%u rx_head=%u rx_tail=%u raw=%08x status=%08x fsm=%08x tx_waits=%u tx_timeouts=%u\n",
                         g_uart_last_reg,g_uart_last_rx_used,g_uart_last_fault[0],g_uart_last_fault[1],
                         g_uart_last_fault[2],g_uart_last_fault[3],g_uart_last_fault[7],g_uart_last_fault[8],
                         g_uart_last_fault[9],g_uart_tx_waits,g_uart_tx_timeouts);

                  body_wire_fault_summary();body_samples_dump();body_bus_fault_trace();
                  printf("[Body] motor fault id=%d; torque and motor rail off\n", id);
                  break;
                }
            }
        }
      else if (!nod_stage && !release_at)
        {
          motor_bad[0] = motor_bad[1] = 0;
        }
      if (!g_sleeping && g_auto && g_py_ok && (last_state != state || now - last_led >= 250))
        {
          uint8_t level = 8 + (now / 250 % 5) * 4;
          switch (state)
            {
              case 1: leds(0, 12, 40); break;
              case 2: leds(level, 0, level); break;
              case 3: leds(0, level, 8); break;
              default: if (last_state != state) leds(0, 8, 2); break;
            }
          if (last_state != state) printf("[Body] voice=%d lights=auto\n", state);
          last_led = now;
        }
      last_state = state;
      if (g_touch_ok)
        {
          uint8_t value;
          if (rd(SI12, 0x10, &value) >= 0)
            {
              bool down = (value & 0x3f) != 0;
              g_touch_raw = value;
              touch_errors = 0;
              if (!down && settle < 4 && ++settle >= 4) armed = true;
              if (armed && down && !touched && now - last_press >= 600)
                {
                  pthread_mutex_lock(&g_lock);
                  g_touch_event = 1; g_presses++;
                  pthread_mutex_unlock(&g_lock);
                  last_press = now;
                  printf("[Body] head press raw=%02x event=%u\n", value, g_presses);
                  if (nod_stage || release_at)
                    {
                      release_servos(); nod_stage = 0; release_at = 0;
                      g_pos[0]=position(1);g_pos[1]=position(2);
                      printf("[Body] touch interrupted yaw=%d pitch=%d peak_load=%u,%u peak_current=%u,%u\n",
                             g_pos[0],g_pos[1],g_peak_load[0],g_peak_load[1],g_peak_current[0],g_peak_current[1]);
                    }
                }
              if (touched && !down) g_releases++;
              touched = down;
            }
          else if (++touch_errors >= 3)
            {
              g_touch_ok = false;
              printf("[Body] touch disabled after bus errors\n");
            }
        }
      body_loop_wait(&loop_deadline,loop_clock_valid);
    }
  release_servos();
  if (g_uart >= 0) close(g_uart);
  g_uart = -1;
  if (g_py_ok) leds(0, 0, 0);
  pthread_mutex_lock(&g_lock);
  g_started = false;
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

int stackchan_body_start(void)
{
  pthread_t thread;
  pthread_attr_t attr;
  int ret;
  pthread_mutex_lock(&g_lock);
  if (g_started) { pthread_mutex_unlock(&g_lock); return 0; }
  g_running = true; g_started = true;
  ret=body_thread_attributes(&attr);
  if(!ret)
    {
      ret = pthread_create(&thread, &attr, body_worker, NULL);
      pthread_attr_destroy(&attr);
    }
  if (ret) { g_started = false; g_running = false; }
  pthread_mutex_unlock(&g_lock);
  return -ret;
}

void stackchan_body_stop(void)
{
  g_running = false;
}

int stackchan_body_take_touch(void)
{
  int event;
  pthread_mutex_lock(&g_lock);
  event = g_touch_event; g_touch_event = 0;
  pthread_mutex_unlock(&g_lock);
  return event;
}

/* Do not use ROM strtol here. Its overflow/errno path faults in this
 * NuttX environment. Check the small allowed range before multiplying. */
static int decimal_arg(const char *text, int min, int max, int *result)
{
  const unsigned char *p = (const unsigned char *)text;
  unsigned value = 0;
  unsigned limit;
  bool negative = false;
  if (*p == '-' || *p == '+') negative = *p++ == '-';
  if (!*p) return -EINVAL;
  limit = negative ? (unsigned)-min : (unsigned)max;
  do
    {
      unsigned digit;
      if (*p < '0' || *p > '9') return -EINVAL;
      digit = *p++ - '0';
      if (value > limit / 10 ||
          (value == limit / 10 && digit > limit % 10)) return -ERANGE;
      value = value * 10 + digit;
    }
  while (*p);
  *result = negative ? -(int)value : (int)value;
  return 0;
}

int stackchan_body_command(int argc, char **argv)
{
  if(argc==2 && !strcmp(argv[0],"ackprobe"))
    {
     if(strcmp(argv[1],"status"))
       {if(strcmp(argv[1],"on") && strcmp(argv[1],"off"))return -EINVAL;
        bool on=!strcmp(argv[1],"on");pthread_mutex_lock(&g_lock);
        int rc=(g_motion || g_prepare_pending || g_feedback_test || (on && (g_power_state!=BODY_READY || !g_servo_ok)))?-EBUSY:0;
        if(!rc)g_ackprobe_enabled=on;pthread_mutex_unlock(&g_lock);
        printf("[BodyAckProbeConfig] enabled=%u rc=%d no_motion_requested=1\n",(unsigned)g_ackprobe_enabled,rc);return rc;}
     printf("[BodyAckProbeStatus] enabled=%u events=%u id=%d expected=%d observed=%d goal_rc=%d health_rc=%d,%d healthy_mask=%u goal_register_proven=%u elapsed_ms=%u diagnostic_only=0\n",(unsigned)g_ackprobe_enabled,(unsigned)g_ackprobe_events,(int)g_ackprobe_id,(int)g_ackprobe_expected,(int)g_ackprobe_observed,(int)g_ackprobe_goal_rc,(int)g_ackprobe_health_rc0,(int)g_ackprobe_health_rc1,(unsigned)g_ackprobe_valid_mask,(unsigned)g_ackprobe_applied,(unsigned)g_ackprobe_elapsed);return 0;
    }
  if(argc==2 && !strcmp(argv[0],"txproof"))
    {
      int rc=0;if(strcmp(argv[1],"status")){
       if(strcmp(argv[1],"on")&&strcmp(argv[1],"off"))return -EINVAL;
       bool on=!strcmp(argv[1],"on");pthread_mutex_lock(&g_lock);
       rc=(g_motion||g_prepare_pending||g_feedback_test||g_txmirror_enabled||
           (on&&g_power_state==BODY_FAULTED))?-EBUSY:esp32s3_uart1_txproof_config(on);
       if(rc==0)g_txproof_enabled=on;pthread_mutex_unlock(&g_lock);
      }
      pthread_mutex_lock(&g_wire_view_lock);struct body_bus_trace v=g_wire_lastview;
      unsigned n=g_wire_view_number;bool ready=g_wire_view_ready;pthread_mutex_unlock(&g_wire_view_lock);
      printf("[BodyTXProof] enabled=%d rc=%d ready=%d n=%u id=%d reg=%d UART_rc=%d observer_rc=%d bytes=%d exact_packet_copies=%d errors=%x first_difference=%d TX6_RXonly_XTAL40MHz=1\n",g_txproof_enabled,rc,ready,n,v.id,v.reg,v.rc,v.mirror_rc,v.mirror_count,v.mirror_match,v.mirror_errors,v.mirror_first_diff);
      return rc;
    }
  if(argc==2 && !strcmp(argv[0],"scope"))
    {
      int rc=0;
      if(strcmp(argv[1],"status"))
        {bool on=!strcmp(argv[1],"on");if(!on && strcmp(argv[1],"off"))return -EINVAL;
         pthread_mutex_lock(&g_lock);
         rc=(g_motion || g_prepare_pending || g_feedback_test || (on && g_power_state==BODY_FAULTED))?-EBUSY:esp32s3_uart1_scope_config(on);
         pthread_mutex_unlock(&g_lock);}
      uint32_t v[12];esp32s3_uart1_scope_status(v,12);
      printf("[BodyScope] on=%u generation=%u trace=%u id=%u reg=%u bytes=%u frozen=%u TX_words=%u RX_words=%u rc=%d physical_scope_only=1\n",v[0],v[1],v[2],v[3],v[4],v[5],v[11],v[8],v[9],rc);return rc;
    }
  if(argc==2 && !strcmp(argv[0],"observe"))
    {
      if(!strcmp(argv[1],"status"))
        {printf("[BodyObserve] armed=%d timing=%d counters=%d movement_commands=0\n",g_observe_armed,esp32s3_uart1_timing_enabled(),esp32s3_uart1_wire_counter_enabled());return 0;}
      if(strcmp(argv[1],"on") && strcmp(argv[1],"off"))return -EINVAL;
      pthread_mutex_lock(&g_lock);int rc=0;bool on=!strcmp(argv[1],"on");
      if(g_motion || g_prepare_pending || g_feedback_test)rc=-EBUSY;
      else if(on)g_observe_armed=true;
      else
        {g_observe_armed=false;(void)esp32s3_uart1_scope_config(false);int a=esp32s3_uart1_wire_counter_config(false);int b=esp32s3_uart1_timing_config(false);rc=a<0?a:b;}
      pthread_mutex_unlock(&g_lock);
      printf("[BodyObserve] request=%d armed=%d rc=%d power_or_motion_requested=0\n",on,g_observe_armed,rc);return rc;
    }
  if(argc==2 && !strcmp(argv[0],"rxpoll"))
    {
      bool enabled=!strcmp(argv[1],"on");
      if(!enabled && strcmp(argv[1],"off"))return -EINVAL;
      pthread_mutex_lock(&g_lock);
      int rc=-EACCES;
      extern int esp32s3_uart1_rxpoll_select(bool);
      extern int esp32s3_uart1_rxpoll_metrics(uint32_t v[3]);
      if((g_power_state==BODY_OFF_IDLE || (!enabled && g_power_state==BODY_FAULTED)) &&
         !g_motion && !g_servo_ok && !g_prepare_pending)
        rc=esp32s3_uart1_rxpoll_select(enabled);
      uint32_t v[3]={0};esp32s3_uart1_rxpoll_metrics(v);
      pthread_mutex_unlock(&g_lock);
      printf("[BodyRXPoll] enabled=%u rc=%d service_calls=%u bytes=%u diagnostic_only=1\n",v[0],rc,v[1],v[2]);
      return rc;
    }

  if (!argc || !strcmp(argv[0], "status"))
    {
      printf("[BodyClock] period_us=20000 absolute_sleeps=%u fallbacks=%u no_catchup=1\n",g_body_absolute_sleeps,g_body_clock_fallbacks);
      printf("[BodyOfficialFlow] feedback_interval_ms=50 feedback_before_goal=1 initial_ACK=%u fresh_goal_proof=%u live_health_reads=%u health_cache_uses=%u unchanged_updates_sent=%u final_poses_readback_verified=%u final_axes_readback_verified=%u\n",g_official_initial_ACK,g_official_fresh_proof,g_official_health_reads,g_official_cached_health,g_official_unchanged_sent,g_official_final_poses,g_official_final_axes);
      printf("[BodyOfficialFeedback] unavailable=%u single_READ=1 interval_ms=50 no_health_claim=1\n",g_official_feedback_unavailable);
      printf("[BodyOfficialAck] transmitted=%u ACK_confirmed=%u unacknowledged=%u deferred_final_readback=1 immediate_probe_reads=0 goal_resends=0\n",g_official_transmitted,g_official_initial_ACK,g_official_unacknowledged);
      printf("[Body] running=%d py32=%d uid=%04x version=%02x bmi=%02x touch=%d raw=%02x\n",
             g_started, g_py_ok, g_uid, g_version, g_bmi_id, g_touch_ok, g_touch_raw);
      printf("[Body] servo=%d motion=%d yaw=%d pitch=%d home=%d,%d voice=%d auto=%d\n",
             g_servo_ok, g_motion, g_pos[0], g_pos[1], g_home[0], g_home[1], g_voice, g_auto);
      printf("[BodyPowerState] state=%d prepare_pending=%d sleeping=%d fault_latched=%d\n",g_power_state,g_prepare_pending,g_sleeping,g_power_state==BODY_FAULTED);
      printf("[BodySync] committed=%u goals=%u health=%u off=%u failed=%u window_active=%d\n",g_sync_committed_frames,g_sync_verified_goals,g_sync_health_reads,g_sync_verified_off,g_sync_failed_frames,g_sync_window.active);
      printf("[BodyReadRecovery] requests_repeated=%u recovered=%u max_repeats=2 repeat_at_ms=10,30 original_arrival_ms=60 original_parse_ms=100 read_layer_motion_retry=0\n",g_read_repeats,g_read_recovered);
  printf("[BodyGoalRecovery] repeated_frames=%u repeated_targets=%u recovered_frames=%u max_absolute_resend=1 new_step_before_verify=0 retry_after_fault=0\n",g_goal_repeat_frames,g_goal_repeat_targets,g_goal_repeat_recovered);
  uint32_t hd[8];
  if(esp32s3_uart1_halfduplex_metrics(hd,8)==0)
    printf("[BodyHalfDuplex] drive=%u release=%u errors=%u last_drive=%u last_OE=%u last_high=%u last_route=%x parked=%u RX_release_after_complete_TX=1\n",hd[0],hd[1],hd[2],hd[3],hd[4],hd[5],hd[6],hd[7]);
  printf("[BodyGoalTraffic] requested_target_updates=%u targets_protocol_confirmed=%u legacy_unchanged_verified=%u confirmed_goal_packets=%u motion_retry=0\n",g_goal_staged,g_goal_applied,g_goal_unchanged_verified,g_goal_write_packets);
      printf("[BodyPinLifecycle] tx_parked=%d\n",esp32s3_uart1_pins_parked());
      extern int esp32s3_uart1_bus_guard_metrics(uint32_t v[5]);uint32_t guard[5];
      if(esp32s3_uart1_bus_guard_metrics(guard)==0)printf("[BodyBusGuard] calls=%u max_us=%u last_RX_age_us=%u last_TX_age_us=%u timeouts=%u quiet_us=1000 IRQ_enabled=1\n",guard[0],guard[1],guard[2],guard[3],guard[4]);
      printf("[BodyTXWait] waits=%u timeouts=%u deadline_ms=20\n",g_uart_tx_waits,g_uart_tx_timeouts);
      extern int esp32s3_uart1_txwire_metrics(uint32_t v[4]);uint32_t wire_us[4];
      if(esp32s3_uart1_txwire_metrics(wire_us)==0)printf("[BodyTXWire] calls=%u max_us=%u last_us=%u timeouts=%u deadline_us=20000 IRQ_enabled=1\n",wire_us[0],wire_us[1],wire_us[2],wire_us[3]);
      printf("[BodyUART] buffered_ontime=%u late_rejected=%u parse_expired=%u\n",
             atomic_load(&g_uart_buffered_ontime),
             atomic_load(&g_uart_late_rejected),
             atomic_load(&g_uart_parse_expired));
      printf("[Body] presses=%u releases=%u led_updates=%u moves=%u i2c_errors=%u uart_errors=%u motor_faults=%u\n",
             g_presses, g_releases, g_light_updates, g_moves, g_i2c_errors, g_uart_errors, g_motor_faults);
      return 0;
    }
  if(!strcmp(argv[0],"wirecounter") && argc==2 && !strcmp(argv[1],"status"))
    {pthread_mutex_lock(&g_wire_view_lock);struct body_bus_trace t=g_wire_lastview;
     unsigned n=g_wire_view_number;bool ready=g_wire_view_ready;pthread_mutex_unlock(&g_wire_view_lock);
     printf("[BodyWireSample] ready=%d enabled=%d n=%u id=%d reg=%d UART_rc=%d counter_rc=%d tx=%d expected_tx=%d rx=%d expected_rx=%d\n",
            ready,esp32s3_uart1_wire_counter_enabled(),n,t.id,t.reg,t.rc,t.wire_rc,t.tx_edges,t.expected_tx,t.rx_edges,t.expected_rx);return 0;}
  if(!strcmp(argv[0],"rxmirror") && argc==2)
    {
     if(!strcmp(argv[1],"status"))
       {struct body_bus_trace v;unsigned n;bool ready;
        pthread_mutex_lock(&g_wire_view_lock);v=g_wire_lastview;n=g_wire_view_number;ready=g_wire_view_ready;pthread_mutex_unlock(&g_wire_view_lock);
        printf("[BodyRXMirror] ready=%d enabled=%d n=%u id=%d reg=%d UART_rc=%d observer_rc=%d bytes=%d packet_valid=%d errors=%x seen_by_arrival=%d\n",ready,g_txmirror_enabled,n,v.id,v.reg,v.rc,v.mirror_rc,v.mirror_count,v.mirror_match,v.mirror_errors,v.mirror_first_diff);return 0;}
     if(strcmp(argv[1],"on") && strcmp(argv[1],"off"))return -EINVAL;
     pthread_mutex_lock(&g_lock);bool on=!strcmp(argv[1],"on");int rc;
     if(g_motion || g_prepare_pending || g_feedback_test || (on && (g_power_state!=BODY_READY || !g_servo_ok)))rc=-EBUSY;
     else if(g_txproof_enabled)rc=-EBUSY;else rc=esp32s3_uart1_txmirror_config(on);
     if(rc==0)g_txmirror_enabled=on;
     pthread_mutex_unlock(&g_lock);printf("[BodyRXMirrorConfig] enabled=%d rc=%d UART1_unchanged=1\n",g_txmirror_enabled,rc);return rc;
    }
  if(!strcmp(argv[0],"wirecounter") && argc==2)
    {if(!g_started)return -ENODEV;
     if(strcmp(argv[1],"on") && strcmp(argv[1],"off"))return -EINVAL;
     pthread_mutex_lock(&g_lock);bool on=!strcmp(argv[1],"on");int rc;
     if(g_motion || g_prepare_pending || g_feedback_test || (on && g_power_state==BODY_FAULTED))rc=-EBUSY;
     else rc=esp32s3_uart1_wire_counter_config(on);
     int enabled=esp32s3_uart1_wire_counter_enabled();pthread_mutex_unlock(&g_lock);
     printf("[BodyWireCounter] enabled=%d rc=%d UART_unchanged=1\n",enabled,rc);return rc;}
  if (!g_started) return -ENODEV;
  if(!strcmp(argv[0],"timing") && argc==2)
    {if(!strcmp(argv[1],"status")){printf("[BodyTiming] enabled=%d\n",esp32s3_uart1_timing_enabled());return 0;}
     if(strcmp(argv[1],"on") && strcmp(argv[1],"off"))return -EINVAL;
     bool on=!strcmp(argv[1],"on");pthread_mutex_lock(&g_lock);int rc;
     if(g_motion || g_prepare_pending || g_feedback_test || (on && (g_power_state!=BODY_READY || !g_servo_ok)))rc=-EBUSY;
     else rc=esp32s3_uart1_timing_config(on);
     pthread_mutex_unlock(&g_lock);printf("[BodyTiming] config=%d enabled=%d rc=%d\n",on,esp32s3_uart1_timing_enabled(),rc);return rc;}
  pthread_mutex_lock(&g_lock);
  if(g_prepare_pending)
    {
      if(!strcmp(argv[0],"stop") || !strcmp(argv[0],"poweroff"))
        {g_prepare_cancel=true;g_request|=REQ_STOP;pthread_mutex_unlock(&g_lock);return 0;}
      pthread_mutex_unlock(&g_lock);return -EBUSY;
    }
  if(!strcmp(argv[0],"prepare") && argc==1)
    {
      if(g_power_state==BODY_READY && g_servo_ok){pthread_mutex_unlock(&g_lock);return 0;}
      if(!body_power_may_prepare(g_power_state)){pthread_mutex_unlock(&g_lock);return -EACCES;}
      g_prepare_cancel=false;g_prepare_done=false;g_prepare_pending=true;g_request|=REQ_PREPARE;
      printf("[BodyPrepare] queued explicit=1 motion=0\n");
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if(g_feedback_test)
    {
      if(!strcmp(argv[0],"stop") || !strcmp(argv[0],"poweroff"))
        {g_feedback_cancel=true;pthread_mutex_unlock(&g_lock);return 0;}
      pthread_mutex_unlock(&g_lock);return -EBUSY;
    }
  if(((!strcmp(argv[0],"buscheck") || !strcmp(argv[0],"stagecheck")) && argc==1) || (!strcmp(argv[0],"readstress") && argc==2 && (!strcmp(argv[1],"0") || !strcmp(argv[1],"2000"))) || (!strcmp(argv[0],"holdcheck") && argc==2 && (!strcmp(argv[1],"1") || !strcmp(argv[1],"2"))))
    {
      if(g_power_state!=BODY_READY || !g_servo_ok || g_motion || g_sleeping)
        {pthread_mutex_unlock(&g_lock);return -EACCES;}
      g_holdcheck_axis=!strcmp(argv[0],"holdcheck")?(unsigned)atoi(argv[1]):0;g_buscheck_readonly=!strcmp(argv[0],"readstress");g_readstress_gap_us=g_buscheck_readonly?(unsigned)atoi(argv[1]):0;g_buscheck_staged=!strcmp(argv[0],"stagecheck");g_feedback_cancel=false;g_feedback_test=true;g_request=(g_request&(REQ_LED|REQ_AUTO))|REQ_BUS_CHECK;
      printf("[BodyBusCheck] queued explicit=1 cycles=128 readonly=%u gap_us=%u torque_on=0 executed_position_writes=0\n",g_buscheck_readonly,g_readstress_gap_us);
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if(!strcmp(argv[0],"startuptest") && argc==1)
    {
      if(g_motion || g_servo_ok || g_sleeping){pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_feedback_cancel=false;g_feedback_test=true;g_request=REQ_STARTUP;
      printf("[BodyStartup] queued explicit=1 motion=0 ready=0\n");
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if(!strcmp(argv[0],"powerpath") && argc==1)
    {
      if(g_motion || g_servo_ok || g_sleeping){pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_feedback_cancel=false;g_feedback_test=true;g_request=REQ_POWER_PATH;
      printf("[BodyPowerPath] queued explicit=1 motion=0 ready=0\n");
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if(!strcmp(argv[0],"ledbus") && argc==1)
    {
      if(g_motion || g_servo_ok){pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_feedback_cancel=false;g_feedback_test=true;
      g_request=REQ_LED_BUS;
      printf("[BodyLedBus] queued motor_power=off\n");
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if(!strcmp(argv[0],"uartcompare") && argc==1)
    {
      if(g_motion || g_servo_ok){pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_feedback_cancel=false;g_feedback_test=true;g_motion=false;g_servo_ok=false;
      g_request=(g_request&(REQ_LED|REQ_AUTO))|REQ_UART_COMPARE;
      printf("[BodyRxCompare] queued read_only=1\n");
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if(!strcmp(argv[0],"uartreceive") && argc==2)
    {
      if(strcmp(argv[1],"off") && strcmp(argv[1],"on"))goto invalid;
      if(g_motion || g_servo_ok){pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_feedback_powered=!strcmp(argv[1],"on");g_feedback_cancel=false;g_feedback_test=true;
      g_motion=false;g_servo_ok=false;g_request=(g_request&(REQ_LED|REQ_AUTO))|REQ_UART_RECEIVE;
      printf("[BodyRxPad] queued powered=%d read_only=1\n",g_feedback_powered);
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if(!strcmp(argv[0],"uartwave") && argc==1)
    {
      if(g_motion || g_servo_ok){pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_feedback_powered=false;g_feedback_cancel=false;g_feedback_test=true;
      g_motion=false;g_servo_ok=false;
      g_request=(g_request&(REQ_LED|REQ_AUTO))|REQ_UART_WAVE;
      printf("[BodyUartWave] queued motor_power=off motion=0 ready=0\n");
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if(!strcmp(argv[0],"uarttest") && argc==1)
    {
      if(g_motion || g_servo_ok){pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_feedback_powered=false;g_feedback_cancel=false;g_feedback_test=true;
      g_motion=false;g_servo_ok=false;
      g_request=(g_request&(REQ_LED|REQ_AUTO))|REQ_UART_TEST;
      printf("[BodyUartTest] queued motor_power=off motion=0 ready=0\n");
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if(!strcmp(argv[0],"feedbacktest") && argc==2)
    {
      if(strcmp(argv[1],"off") && strcmp(argv[1],"on"))goto invalid;
      if(g_motion || g_servo_ok){pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_feedback_powered=!strcmp(argv[1],"on");
      g_feedback_cancel=false;g_feedback_test=true;
      g_motion=false;g_servo_ok=false;
      g_request=(g_request&(REQ_LED|REQ_AUTO))|REQ_FEEDBACK_TEST;
      printf("[BodyFeedback] queued powered=%d motion=0 ready=0\n",g_feedback_powered);
      pthread_mutex_unlock(&g_lock);return 0;
    }
  if (!strcmp(argv[0], "poweroff")) g_request |= REQ_POWER_OFF;
  else if (!strcmp(argv[0], "touchtest"))
    {
      g_touch_event = 2;
      printf("[Body] synthetic head event (not physical sensor evidence)\n");
    }
  else if (!strcmp(argv[0],"powertrace") && argc==2)
    {if(strcmp(argv[1],"on") && strcmp(argv[1],"off"))goto invalid;
     g_power_trace=!strcmp(argv[1],"on");printf("[Body] powertrace=%d\n",g_power_trace);}
  else if (!strcmp(argv[0],"focus") && argc==2)
    {if(strcmp(argv[1],"on") && strcmp(argv[1],"off"))goto invalid;
     g_focused=!strcmp(argv[1],"on");if(!g_focused)g_request|=REQ_RELEASE;
     printf("[Body] diagnostic_focus=%d\n",g_focused);}
  else if (!strcmp(argv[0],"hold") && argc==2)
    {if(strcmp(argv[1],"on") && strcmp(argv[1],"off"))goto invalid;
     g_focus_hold=!strcmp(argv[1],"on");
     if(!g_focus_hold && g_settled)g_request|=REQ_RELEASE;
     printf("[Body] hold_focused=%d\n",g_focus_hold);}
  else if (!strcmp(argv[0],"pose"))g_request|=REQ_POSE;
  else if (!strcmp(argv[0], "probe")) g_request |= REQ_PROBE;
  else if (!strcmp(argv[0], "diag")) g_request |= REQ_DIAG;
  else if (!strcmp(argv[0], "auto")) { g_auto = true; g_request |= REQ_AUTO; }
  else if (!strcmp(argv[0], "stop")) g_request |= REQ_STOP;
  else if (!strcmp(argv[0], "motion") && argc == 2)
    {
      if (!strcmp(argv[1], "on") && g_servo_ok) g_motion = true;
      else if (!strcmp(argv[1], "off")) g_request |= REQ_STOP;
      else goto invalid;
    }
  else if (!strcmp(argv[0], "nod") && g_motion && g_servo_ok)
    g_request |= REQ_NOD;
  else if(!strcmp(argv[0],"axis") && argc==3)
    {
      int axis,angle;
      if(decimal_arg(argv[1],1,2,&axis)<0 || decimal_arg(argv[2],axis==1?-18:-10,axis==1?18:12,&angle)<0)goto invalid;
      if(g_power_state!=BODY_READY || !g_servo_ok || !g_motion || g_sleeping || !g_settled || (g_request&REQ_MOVE))
        {pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_diag_axis=axis;g_target_yaw=axis==1?angle:0;g_target_pitch=axis==2?angle:0;
      g_request|=REQ_MOVE|REQ_AXIS;
      printf("[BodyAxisRequest] axis=%d degrees=%d explicit_diagnostic=1\n",axis,angle);
    }
  else if (!strcmp(argv[0], "look") && argc == 3 && g_motion && g_servo_ok)
    {
      int yaw, pitch;
      if (decimal_arg(argv[1], BODY_YAW_MIN, BODY_YAW_MAX, &yaw) < 0 ||
          decimal_arg(argv[2], BODY_PITCH_MIN, BODY_PITCH_MAX, &pitch) < 0) goto invalid;
      g_target_yaw = yaw; g_target_pitch = pitch; g_request=(g_request&~REQ_AXIS)|REQ_MOVE;
    }
  else if (!strcmp(argv[0], "led") && argc == 4)
    {
      int i;
      for (i = 0; i < 3; i++)
        {
          int v;
          if (decimal_arg(argv[i + 1], 0, 64, &v) < 0) goto invalid;
          g_rgb[i] = v;
        }
      g_auto = false; g_request |= REQ_LED;
    }
  else goto invalid;
  pthread_mutex_unlock(&g_lock);
  return 0;
invalid:
  pthread_mutex_unlock(&g_lock);
  printf("Usage: plant body status|poweroff|probe|diag|feedbacktest off/on|uarttest|touchtest|auto|led R G B (0..64)|motion on/off|look yaw(-120..120) pitch(-15..45 relative to 20deg)|nod|stop\n");
  return -EINVAL;
}


/* Call only from an interaction worker, never the LVGL tick. */
int stackchan_body_prepare(void)
{
  if(!g_started || !g_bus)return -ENODEV;
  pthread_mutex_lock(&g_lock);
  if(g_feedback_test || g_prepare_pending){pthread_mutex_unlock(&g_lock);return -EBUSY;}
  if(g_power_state==BODY_READY && g_servo_ok){pthread_mutex_unlock(&g_lock);return 0;}
  if(!body_power_may_prepare(g_power_state) || g_sleeping){pthread_mutex_unlock(&g_lock);return -EACCES;}
  g_prepare_cancel=false;g_prepare_done=false;g_prepare_pending=true;g_request|=REQ_PREPARE;
  pthread_mutex_unlock(&g_lock);
  for(unsigned n=0;n<BODY_PREPARE_WAIT_MS/5 && !g_prepare_done;n++)usleep(5000);
  if(!g_prepare_done){g_prepare_cancel=true;return -ETIMEDOUT;}
  return g_prepare_rc;
}

int stackchan_body_get_pose(struct stackchan_pose *p)
{
  if(!p)return -EINVAL;
  p->yaw=g_pos[0];p->pitch=g_pos[1];p->home_yaw=g_home[0];p->home_pitch=g_home[1];
  p->settled=g_settled;p->ready=g_servo_ok;return p->ready?0:-ENODEV;
}
int stackchan_body_look(int yaw,int pitch)
{
  if(yaw<BODY_YAW_MIN || yaw>BODY_YAW_MAX || pitch<BODY_PITCH_MIN || pitch>BODY_PITCH_MAX)return -ERANGE;
  pthread_mutex_lock(&g_lock);
  if(g_feedback_test){pthread_mutex_unlock(&g_lock);return -EBUSY;}
  if(!g_servo_ok || g_power_state!=BODY_READY || g_sleeping){int rc=g_power_state==BODY_FAULTED?-EHOSTDOWN:-ENODEV;pthread_mutex_unlock(&g_lock);return rc;}
  g_motion=true;g_settled=false;g_target_yaw=yaw;g_target_pitch=pitch;g_request=(g_request&~REQ_AXIS)|REQ_MOVE;
  pthread_mutex_unlock(&g_lock);return 0;
}
/* User direction commands own one axis; the other gets fresh health checks
 * but no new goal, preserving its actual/raw state without a degree roundtrip. */
int stackchan_body_look_axis(int axis,int angle)
{
  if(axis<1 || axis>2)return -EINVAL;
  if(angle<(axis==1?BODY_YAW_MIN:BODY_PITCH_MIN) || angle>(axis==1?BODY_YAW_MAX:BODY_PITCH_MAX))return -ERANGE;
  pthread_mutex_lock(&g_lock);
  if(g_feedback_test){pthread_mutex_unlock(&g_lock);return -EBUSY;}
  if(!g_servo_ok || g_power_state!=BODY_READY || g_sleeping){int rc=g_power_state==BODY_FAULTED?-EHOSTDOWN:-ENODEV;pthread_mutex_unlock(&g_lock);return rc;}
  g_motion=true;g_settled=false;g_diag_axis=axis;
  g_target_yaw=axis==1?angle:0;g_target_pitch=axis==2?angle:0;
  g_request|=REQ_MOVE|REQ_AXIS;
  pthread_mutex_unlock(&g_lock);return 0;
}
int stackchan_body_look_raw(int yaw,int pitch)
{
  if(yaw<0 || yaw>1023 || pitch<0 || pitch>1023)return -EINVAL;
  if(!body_raw_valid(1,yaw)||!body_raw_valid(2,pitch))return -ERANGE;
  return stackchan_body_look((yaw-g_home[0])*5/16,(pitch-g_home[1])*5/16);
}
void stackchan_body_focus(bool enabled)
{
  g_focused=enabled;
  if(!enabled){pthread_mutex_lock(&g_lock);g_request|=REQ_RELEASE;pthread_mutex_unlock(&g_lock);}
}

int stackchan_body_idle_power(void)
{
  if(!g_started)return -ENODEV;
  pthread_mutex_lock(&g_lock);
  if(g_feedback_test){pthread_mutex_unlock(&g_lock);return -EBUSY;}
  g_prepare_cancel=true;g_sleeping=true;g_idle_done=false;g_idle_rc=-EINPROGRESS;
  g_request=REQ_IDLE_POWER;pthread_mutex_unlock(&g_lock);
  for(int i=0;i<100&&!g_idle_done;i++)usleep(10000);
  return g_idle_done?g_idle_rc:-ETIMEDOUT;
}
void stackchan_body_idle_wake(void){g_sleeping=false;}
