/* Bounded diagnostic: only READs while both torque registers are verified OFF.
 * Never calls a position write, torque-ON, ACTION or a motor retry. */
static int body_readstress_core(unsigned cycles,unsigned gap_us,unsigned *completed)
{
  if(!completed || !cycles || cycles>128 || (gap_us!=0 && gap_us!=2000))return -EINVAL;
  *completed=0;
  if(g_power_state!=BODY_READY || !g_servo_ok || g_motion || g_sync_window.active)return -EACCES;
  if(!g_running || g_sleeping || g_feedback_cancel)return -ECANCELED;
  uint32_t start=now_ms();
  for(unsigned id=1;id<=2;id++)
    {uint8_t enabled=255;int rc=servo_read(id,40,&enabled,1);
     if(rc<0 || enabled)return body_sync_failed(rc<0?rc:-EACCES,30,id,0,enabled);}
  for(unsigned n=0;n<cycles;n++)
    {
      if(!g_running || g_sleeping || g_feedback_cancel)return -ECANCELED;
      if((uint32_t)(now_ms()-start)>=8000)return body_sync_failed(-ETIMEDOUT,31,0,0,0);
      g_sync_window.started=now_ms();g_sync_window.active=true;
      for(unsigned id=1;id<=2;id++)
        {
          if(gap_us)usleep(gap_us); /* Included in original frame budget. */
          if(!g_running || g_sleeping || g_feedback_cancel)return -ECANCELED;
          if(!body_sync_budget(&g_sync_window,now_ms(),60))return body_sync_failed(-ETIMEDOUT,31,id,0,0);
          int goal=-1;bool healthy=motor_goal_feedback_ok(id,&goal);
          if(g_feedback_rc[id-1]<0)return body_sync_failed(g_feedback_rc[id-1],32,id,0,goal);
          if(!body_servo_target_valid(id,goal))return body_sync_failed(-ERANGE,33,id,0,goal);
          if(!healthy)
            {
              usleep(20000);healthy=motor_goal_feedback_ok(id,&goal);
              if(!healthy)return body_sync_failed(g_feedback_rc[id-1]<0?g_feedback_rc[id-1]:-ERANGE,34,id,0,goal);
            }
        }
      g_sync_window.active=false;(*completed)++;
    }
  for(unsigned id=1;id<=2;id++)
    {uint8_t enabled=255;int rc=servo_read(id,40,&enabled,1);
     if(rc<0 || enabled)return body_sync_failed(rc<0?rc:-EACCES,35,id,0,enabled);}
  return 0;
}
