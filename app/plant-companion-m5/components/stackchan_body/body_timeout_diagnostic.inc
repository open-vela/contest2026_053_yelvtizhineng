/* Freeze planning BEFORE read-only evidence collection. Last absolute step
 * is bounded by the existing3raw rule; there is no goal/enable resend.
 * Two samples, original per-transaction60/100ms caps, total180ms cap.
 * ALWAYS return to the original error and mandatory power cut afterwards. */
static void body_timeout_diagnose(unsigned id)
{
 if(g_timeout_diag_seen || g_timeout_diag_active || id<1 || id>2 ||
    g_power_state!=BODY_READY || !g_sync_window.active)return;
 g_timeout_diag_seen=true;g_timeout_diag_id=id;g_motion=false;
 body_ramps_cancel(); /* Also disables the original failed frame window. */
 g_timeout_diag_active=true;g_timeout_diag_start=now_ms();
 int old_used=g_uart_last_rx_used,old_reg=g_uart_last_reg;
 uint32_t old_fault[10];memcpy(old_fault,g_uart_last_fault,sizeof(old_fault));
 for(unsigned n=0;n<2;n++)
   {
    unsigned due=50u*(n+1),elapsed=(uint32_t)(now_ms()-g_timeout_diag_start);
    if(elapsed<due)usleep((due-elapsed)*1000);
    elapsed=(uint32_t)(now_ms()-g_timeout_diag_start);
    if(elapsed>=BODY_TIMEOUT_DIAG_BUDGET_MS)break;
    uint8_t p[2]={56,15},feedback[15]={0};
    struct body_timeout_sample *s=&g_timeout_samples[g_timeout_sample_count++];
    s->at=elapsed;s->rc=servo_packet_traced(id,2,p,2,feedback,15);
    s->pos=s->load=s->current=-1;s->healthy=0;
    if(s->rc==0)
      {s->pos=((unsigned)feedback[0]<<8)|feedback[1];
       s->load=(((unsigned)feedback[4]<<8)|feedback[5])&1023;
       s->current=(((unsigned)feedback[13]<<8)|feedback[14])&32767;
       s->healthy=body_feedback_raw_valid(id,s->pos)&&s->load<650&&s->current<350;
       if(!s->healthy)break;}
    else if(s->rc!=-ETIMEDOUT)break;
   }
 g_timeout_diag_elapsed=now_ms()-g_timeout_diag_start;
 memcpy(g_uart_last_fault,old_fault,sizeof(old_fault));
 g_uart_last_rx_used=old_used;g_uart_last_reg=old_reg;
 g_timeout_diag_active=false; /* Caller MUST still cut for the ORIGINAL rc. */
}
static void body_timeout_diagnostic_print(void)
{
 if(!g_timeout_diag_seen || g_timeout_diag_printed)return;
 g_timeout_diag_printed=true;
 printf("[BodyTimeoutHold] id=%u samples=%u elapsed_ms=%u budget_ms=180 new_goal_writes=0 new_enable_writes=0 original_frame_failed=1 diagnostic_only=1\n",g_timeout_diag_id,g_timeout_sample_count,g_timeout_diag_elapsed);
 for(unsigned n=0;n<g_timeout_sample_count;n++)
  {struct body_timeout_sample *s=&g_timeout_samples[n];printf("[BodyTimeoutSample] n=%u at_ms=%u rc=%d pos=%d load=%d current=%d healthy=%d\n",n,s->at,s->rc,s->pos,s->load,s->current,s->healthy);}
}
