/* Explicit stationary six-packet traffic diagnostic. No goal or torque-ON.
 * Normal frame limits and first-failure cut remain in force. */
static int body_buscheck_stage(unsigned id,int target)
{
 if(!body_servo_target_valid(id,target))return -ERANGE;
 uint8_t p[7]={42,target>>8,target,0,20,0,0};
 return servo_packet(id,4,p,7,NULL,0); /* REG_WRITE only. No ACTION exists here. */
}
static int body_buscheck_core(unsigned cycles,unsigned *completed,bool staged)
{
  if(!completed || !cycles || cycles>128)return -EINVAL;
  *completed=0;
  if(g_power_state!=BODY_READY || !g_servo_ok || g_motion)return -EACCES;
  if(!g_running || g_sleeping || g_feedback_cancel)return -ECANCELED;
  if(g_sync_window.active)return body_sync_failed(-EBUSY,20,0,0,0);
  uint32_t started=now_ms();
  if(staged)
    {
     g_sync_window.started=now_ms();g_sync_window.active=true;
    for(unsigned id=1;id<=2;id++)
      {uint8_t enabled=255;int rc=servo_read(id,40,&enabled,1);
       if(rc<0 || enabled!=0)return body_sync_failed(rc<0?rc:-EIO,25,id,0,enabled);}
     g_sync_window.active=false;
    }
  for(unsigned n=0;n<cycles;n++)
    {
      if(!g_running || g_sleeping || g_feedback_cancel)return -ECANCELED;
      if((uint32_t)(now_ms()-started)>=8000)return body_sync_failed(-ETIMEDOUT,20,0,0,0);
      if(g_sync_window.active)return body_sync_failed(-EBUSY,20,0,0,0);
      g_sync_window.started=now_ms();g_sync_window.active=true;
      uint8_t off=0;
      for(unsigned id=1;id<=2;id++)
        {int rc=staged?body_buscheck_stage(id,g_pos[id-1]):servo_write(id,40,&off,1);if(rc<0)return body_sync_failed(rc,21,id,0,0);}
      for(unsigned id=1;id<=2;id++)
        {
          uint8_t enabled=255;int rc=servo_read(id,40,&enabled,1);
          if(rc<0 || enabled!=0)return body_sync_failed(rc<0?rc:-EIO,22,id,0,enabled);
          bool healthy=motor_feedback_ok(id);
          if(!healthy)
            {
              if(g_feedback_rc[id-1]<0)return body_sync_failed(g_feedback_rc[id-1],23,id,0,0);
              usleep(20000);healthy=motor_feedback_ok(id);
              if(!healthy)return body_sync_failed(g_feedback_rc[id-1]<0?g_feedback_rc[id-1]:-ERANGE,24,id,0,0);
            }
        }
      g_sync_window.active=false;(*completed)++;
      if(n+1<cycles)usleep(20000);
    }
  return 0;
}
