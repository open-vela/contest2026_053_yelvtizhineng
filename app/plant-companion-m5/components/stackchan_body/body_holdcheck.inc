/* Explicit diagnostic: official unicast WritePos at the freshly measured
 * current position. Never a new travel goal, and never retry a failed command. */
static int g_hold_target,g_hold_torque;
static unsigned g_hold_requested,g_hold_verified,g_hold_max_deviation;
static int body_holdcheck_core(unsigned cycles,unsigned axis,unsigned *completed)
{
 if(!completed || !cycles || cycles>128 || axis<1 || axis>2)return -EINVAL;
 *completed=0;g_hold_requested=g_hold_verified=g_hold_max_deviation=0;g_hold_torque=-1;
 if(g_power_state!=BODY_READY || !g_servo_ok || g_motion || g_sync_window.active)return -EACCES;
 if(!g_running || g_sleeping || g_feedback_cancel)return -ECANCELED;
 uint32_t start=now_ms();
 for(unsigned id=1;id<=2;id++)
   {uint8_t enabled=255;int rc=servo_read(id,40,&enabled,1);
    if(rc<0 || enabled)return body_sync_failed(rc<0?rc:-EACCES,40,id,0,enabled);}
 int target=position(axis);if(!body_servo_target_valid(axis,target))return body_sync_failed(-ERANGE,40,axis,0,target);
 g_hold_target=target;uint8_t data[6]={(unsigned)target>>8,target,0,20,0,0};
 for(unsigned n=0;n<cycles;n++)
   {
    if(!g_running || g_sleeping || g_feedback_cancel)return -ECANCELED;
    if((uint32_t)(now_ms()-start)>=8000)return body_sync_failed(-ETIMEDOUT,41,axis,0,0);
    g_sync_window.started=now_ms();g_sync_window.active=true;g_hold_requested++;
    int rc=servo_write(axis,42,data,6);if(rc<0)return body_sync_failed(rc,42,axis,target,-1);
    if(n==0)
      {uint8_t torque=255;rc=servo_read(axis,40,&torque,1);
       if(rc<0 || torque>1)return body_sync_failed(rc<0?rc:-EPROTO,43,axis,0,torque);g_hold_torque=torque;}
    for(unsigned id=1;id<=2;id++)
      {
       int goal=-1;bool healthy=id==axis?motor_goal_feedback_ok(id,&goal):motor_feedback_ok(id);
       if(g_feedback_rc[id-1]<0)return body_sync_failed(g_feedback_rc[id-1],44,id,0,goal);
       if(id==axis && goal!=target)return body_sync_failed(-EIO,45,id,target,goal);
       if(!healthy)
         {usleep(20000);healthy=motor_feedback_ok(id);
          if(!healthy)return body_sync_failed(g_feedback_rc[id-1]<0?g_feedback_rc[id-1]:-ERANGE,46,id,0,0);}
      }
    unsigned deviation=abs((int)g_feedback_position[axis-1]-target);
    if(deviation>g_hold_max_deviation)g_hold_max_deviation=deviation;
    if(deviation>12)return body_sync_failed(-ERANGE,47,axis,target,g_feedback_position[axis-1]);
    g_hold_verified++;g_sync_window.active=false;(*completed)++;
    if(n+1<cycles)usleep(20000);
   }
 return 0;
}
