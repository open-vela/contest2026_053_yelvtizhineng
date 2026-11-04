/* A close alternative name is not an unambiguous wake phrase. Keep the
 * original score threshold and accept weak unique candidates as before. */
static bool wake_match_accept(const esp_mn_results_t *r,float minimum,
                              float *competitor,bool *ambiguous)
{
  if(competitor)*competitor=0;
  if(ambiguous)*ambiguous=false;
  if(!r || r->num<=0 || r->command_id[0]!=1)return false;
  float top=r->prob[0],other=0;
  if(!(top>=minimum && top<=1.0f))return false;
  int count=r->num<ESP_MN_RESULT_MAX_NUM?r->num:ESP_MN_RESULT_MAX_NUM;
  for(int i=1;i<count;i++)
    {
      float p=r->prob[i];
      if(r->command_id[i]!=1 && p>=0 && p<=1 && p>other)other=p;
    }
  if(competitor)*competitor=other;
  bool close=other>=top*0.9f;
  if(ambiguous)*ambiguous=close;
  return !close;
}
