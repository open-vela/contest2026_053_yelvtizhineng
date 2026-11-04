/* Required readback is the transaction acknowledgement. All verification
 * shares the original60ms arrival/100ms parse window from the motion frame.
 * No next target or model commit is allowed until it all succeeds. */
static int body_sync_failed(int rc,unsigned phase,unsigned id,int expected,int observed)
{
  g_stage_acked_mask=0;g_sync_failed_frames++;
  int off=-1;
  if(g_power_state!=BODY_FAULTED)off=body_transport_fail_stop();
  g_sync_window.active=false;
  printf("[BodySyncStop] rc=%d phase=%u id=%u expected=%d observed=%d rail_off=%d no_retry_after_fault=1\n",rc,phase,id,expected,observed,off);
  return rc;
}
/* Same ordinary unicast WritePos used by official Hal::ScsServo. Each
 * selected axis needs an ACK or fresh exact goal+both-health proof.
 * One idempotent recovery is allowed only for a proven previous goal, never
 * after a hard error. Second-write ACK and final goal+both health are mandatory. */
static int body_write_position_mask(unsigned mask,const uint8_t data[2][6])
{
  for(unsigned id=1;id<=2;id++)if(mask&(1u<<(id-1)))
    {
      g_ackprobe_wire_id=g_ackprobe_enabled && !g_goal_retry_mask?id:0;
      int rc=servo_write(id,42,data[id-1],6);g_ackprobe_wire_id=0;
      if(rc==BODY_ACKPROBE_PENDING)
        {
          rc=body_ackprobe_after_absence(id,((unsigned)data[id-1][0]<<8)|data[id-1][1],true);
          if(rc==2)
            {
              /* Same absolute target, not another motion step. The proof
               * above found precisely the last committed goal and fresh
               * healthy axes. No new shared window or second recovery. */
              g_goal_retry_mask=1u<<(id-1);g_goal_repeat_frames++;g_goal_repeat_targets++;
              rc=servo_write(id,42,data[id-1],6); /* Strict ACK: not armed. */
              if(rc<0)return rc;
              rc=body_ackprobe_after_absence(id,((unsigned)data[id-1][0]<<8)|data[id-1][1],false);
              if(rc<0)return rc;
              g_goal_write_packets++;
            }
        }
      if(rc<0)return rc;
      g_goal_write_packets++;
    }
  return 0;
}
static int body_sync_verified(unsigned mask,unsigned reg,const uint8_t data[2][6])
{
  if(g_power_state==BODY_FAULTED)return -EHOSTDOWN;
  if(g_power_state!=BODY_READY || !g_servo_ok)return -ENODEV;
  uint8_t params[16];int count=body_sync_encode(mask,reg,data,params);
  if(count<0)return body_sync_failed(count,0,0,0,0);
  if(g_sync_window.active)return body_sync_failed(-EBUSY,0,0,0,0);
  g_sync_window.started=now_ms();g_sync_window.active=true;
  int rc=0;unsigned write_mask=mask,unchanged=0;g_stage_acked_mask=0;g_goal_retry_mask=0;
  if(reg==42)
    {
      /* The first frame must always re-arm its target. Later quantized
       * frames may omit an unchanged goal only after a verified commit.
       * All requested goals and both fresh health blocks are still read
       * below. This never treats a cached goal as hardware verification. */
      for(unsigned id=1;id<=2;id++)if(mask&(1u<<(id-1)))
        {
          if(g_servo_reply_level[id-1]!=1)
            return body_sync_failed(-EPROTO,1,id,1,g_servo_reply_level[id-1]);
          int expected=((unsigned)data[id-1][0]<<8)|data[id-1][1];
          if(g_ramp[id-1].step>0 && expected==g_ramp[id-1].current)
            {write_mask&=~(1u<<(id-1));unchanged++;}
        }
      if(write_mask)
        {
          rc=body_write_position_mask(write_mask,data);
          if(rc<0)return body_sync_failed(rc,1,0,0,0);
          g_goal_staged+=(write_mask&1u)+((write_mask>>1)&1u);
        }

    }
  else for(unsigned id=1;id<=2;id++)if(mask&(1u<<(id-1)))
    {rc=servo_write(id,reg,data[id-1],1);if(rc<0)return body_sync_failed(rc,1,id,0,0);}
  bool repeated=g_goal_retry_mask!=0;
  for(unsigned attempt=0;attempt<2;attempt++)
    {
      unsigned repeat_mask=0;bool first_health_clean=true;
      for(unsigned id=1;id<=2;id++)
    {
      if(reg==40)
        {
          if(mask&(1u<<(id-1)))
            {uint8_t off;rc=servo_read(id,40,&off,1);
             if(rc<0)return body_sync_failed(rc,2,id,0,0);
             if(off)return body_sync_failed(-EIO,3,id,0,off);
             g_sync_verified_goals++;}
          continue;
        }
      bool healthy;
      if(mask&(1u<<(id-1)))
        {
          int observed=-1,expected=((unsigned)data[id-1][0]<<8)|data[id-1][1];
          healthy=motor_goal_feedback_ok(id,&observed);g_sync_health_reads++;
          if(g_feedback_rc[id-1]<0)return body_sync_failed(g_feedback_rc[id-1],2,id,expected,observed);
          if(observed!=expected)
            {
              /* Only a positively identified PREVIOUS verified absolute
               * goal can be recovered. No unknown state, first-frame goal,
               * second resend, relative/incremental command or failed health. */
              if(attempt || g_goal_retry_mask || !(write_mask&(1u<<(id-1))) || !g_ramp[id-1].step ||
                 observed!=g_ramp[id-1].current)
                return body_sync_failed(-EIO,3,id,expected,observed);
              repeat_mask|=1u<<(id-1);
            }
          else g_sync_verified_goals++;
        }
      else {healthy=motor_feedback_ok(id);g_sync_health_reads++;}
      if(!healthy)
        {
          first_health_clean=false;
          if(g_feedback_rc[id-1]<0)return body_sync_failed(g_feedback_rc[id-1],4,id,0,0);
          /* Same second valid health sample, same20ms separation and limits.
           * No motion packet is issued while checking an unhealthy sample. */
          usleep(20000);healthy=motor_feedback_ok(id);g_sync_health_reads++;
          if(!healthy)return body_sync_failed(g_feedback_rc[id-1]<0?g_feedback_rc[id-1]:-ERANGE,5,id,0,0);
        }
    }

      if(!repeat_mask)break;
      if(!first_health_clean)return body_sync_failed(-ERANGE,6,0,0,0);
      /* Idempotent recovery, never a new step. Both axes were just freshly
       * healthy, and the missed goal equals its previously verified value.
       * The shared start is NOT renewed; every post-resend goal/health read
       * still has the same original60ms arrival/100ms parse deadline.
       * No retransmission follows a latched protection or unknown state. */
      if(body_sync_budget(&g_sync_window,now_ms(),60)<=20)
        return body_sync_failed(-ETIMEDOUT,6,0,0,0);
      g_goal_retry_mask=repeat_mask;
      rc=body_write_position_mask(repeat_mask,data);
      if(rc<0)return body_sync_failed(rc,6,0,0,0);
      repeated=true;g_goal_repeat_frames++;

      g_goal_repeat_targets+=(repeat_mask&1u)+((repeat_mask>>1)&1u);
    }
  g_sync_window.active=false;
  if(reg==40)g_sync_verified_off++;
  else {if(repeated)g_goal_repeat_recovered++;g_goal_unchanged_verified+=unchanged;g_goal_applied+=(write_mask&1u)+((write_mask>>1)&1u);}
  return 0;
}
static int body_sync_off(unsigned mask)
{
  const uint8_t zeros[2][6]={{0},{0}};
  return body_sync_verified(mask,40,zeros);
}
#ifdef BODY_OFFICIAL_FLOW_PRESENT
int stackchan_body_official_frame(unsigned,const uint8_t [2][6]);
#endif
static int body_sync_move(unsigned mask,const int target[2],const unsigned duration[2])
{
  uint8_t data[2][6]={{0},{0}};
  for(unsigned i=0;i<2;i++)if(mask&(1u<<i))
    {
      if(!body_servo_target_valid(i+1,target[i]) || duration[i]!=20 ||
         target[i]-g_ramp[i].current>3 || target[i]-g_ramp[i].current< -3)
        return body_sync_failed(-ERANGE,0,i+1,g_ramp[i].current,target[i]);
      data[i][0]=target[i]>>8;data[i][1]=target[i];data[i][2]=0;data[i][3]=20;
    }
  int rc;
#ifdef BODY_OFFICIAL_FLOW_PRESENT
  rc=stackchan_body_official_frame(mask,data);
#else
  rc=body_sync_verified(mask,42,data);
#endif
  if(rc<0)return rc;
  uint32_t confirmed=now_ms();
  for(unsigned i=0;i<2;i++)if(mask&(1u<<i))
    {
      /* Do not add verification time to the official20ms update period.
       * A later physical TX stamp still enforces its own20ms cooldown.
       * No new goal/model is committed before all existing checks pass. */
      uint32_t sent=g_sync_window.started,wire;
      extern bool esp32s3_uart1_goal_stamp_get(unsigned,uint32_t *) __attribute__((weak));
      if(esp32s3_uart1_goal_stamp_get && esp32s3_uart1_goal_stamp_get(i+1,&wire) &&
         (int32_t)(wire-sent)>=0 && (int32_t)(confirmed-wire)>=0)sent=wire;
      body_ramp_commit(&g_ramp[i],target[i],duration[i],sent);
    }
  g_sync_committed_frames++;
  return 0;
}
