#include "handsfree_command.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define HAS(s) (strstr(t,(s))!=NULL)
#define IS(s) (strcmp(t,(s))==0)
size_t hf_normalize(const char *text,char *out,size_t cap)
{
 const unsigned char *p=(const unsigned char *)text;size_t n=0;
 if(!out||!cap)return 0;
 if(!p){out[0]=0;return 0;}
 while(*p){
  size_t k=*p<0x80?1:(*p<0xe0?2:(*p<0xf0?3:4));
  if(k==1&&(*p<=32||strchr(",.!?:;\"'",*p))){p++;continue;}
  if(k==3&&(!strncmp((const char *)p,"，",3)||!strncmp((const char *)p,"。",3)||
    !strncmp((const char *)p,"！",3)||!strncmp((const char *)p,"？",3)||
    !strncmp((const char *)p,"、",3)||!strncmp((const char *)p,"：",3)||
    !strncmp((const char *)p,"“",3)||!strncmp((const char *)p,"”",3))){p+=3;continue;}
  for(size_t i=1;i<k;i++)if(!p[i]){out[0]=0;return 0;}
  if(n+k>=cap){out[0]=0;return 0;}
  memcpy(out+n,p,k);n+=k;p+=k;
 }
 out[n]=0;return n;
}
bool hf_is_awake(const struct hf_gate *g,uint32_t now)
{return g&&g->awake&&(uint32_t)(now-g->touched)<45000;}
void hf_keep_awake(struct hf_gate *g,uint32_t now){g->awake=true;g->touched=now;}
static int number(const char *t)
{
 for(const char *p=t;*p;p++)if(*p>='0'&&*p<='9')return atoi(p);
 const char *digits[]={"零","一","二","三","四","五","六","七","八","九"};
 const char *ten=strstr(t,"十");int a=0,b=0;
 if(ten){for(int i=1;i<=9;i++)if(ten>=t+3&&!strncmp(ten-3,digits[i],3))a=i;
  for(int i=1;i<=9;i++)if(!strncmp(ten+3,digits[i],3))b=i;
  return (a?a:1)*10+b;}
 if(strstr(t,"百"))return 100;
 for(int i=0;i<=9;i++)if(strstr(t,digits[i]))return i;
 return -1;
}
static bool element_query(const char *t)
{
 size_t n=strlen(t);const char *suffix="元素多少";size_t length=strlen(suffix);
 while(n>=3&&(!memcmp(t+n-3,"呀",3)||!memcmp(t+n-3,"啊",3)||!memcmp(t+n-3,"呢",3)||!memcmp(t+n-3,"吧",3)))n-=3;
 return n>=length&&n-length<=12&&!memcmp(t+n-length,suffix,length);
}
/* ASR is an aid for weak acoustic candidates, not an exact-text wake lock.
 * Drop short interjections and tolerate a single same-sound spelling.
 * Ordinary reported speech and other names do not match this weak path. */
static const char *wake_tail(const char *text)
{
 const char *p=text;unsigned fillers=0;
 while(fillers<4 && (!strncmp(p,"哎",3)||!strncmp(p,"诶",3)||
       !strncmp(p,"唉",3)||!strncmp(p,"嗯",3)||!strncmp(p,"啊",3)||!strncmp(p,"喂",3))){p+=3;fillers++;}
 const char *words[4][10]={
  {"你","妳","您",NULL}, {"好","号","浩",NULL},
  {"小","晓",NULL}, {"伴","半","办","班","板","拌","绊",NULL}};
 unsigned changed=0;
 for(unsigned i=0;i<4;i++){
  unsigned j=0;while(words[i][j]&&strncmp(p,words[i][j],3))j++;
  if(!words[i][j] || (j && ++changed>1))return NULL;
  p+=3;
 }
 return p;
}
/* Observed quiet microphone ASR: "mm." and "Yeah." started long answers
 * and covered the next real control. Ignore whole filler utterances only;
 * never filter short plant names/questions or filler-prefixed wake. */
static bool filler_only(const char *text)
{
 char folded[32];size_t n=strlen(text);if(n>=sizeof(folded))return false;
 for(size_t i=0;i<=n;i++)folded[i]=text[i]>='A'&&text[i]<='Z'?text[i]+('a'-'A'):text[i];
 const char *words[]={"mm","hmm","mhm","yeah","um","uh","ok","okay","嗯","嗯嗯","啊","呃"};
 for(size_t i=0;i<sizeof(words)/sizeof(words[0]);i++)if(!strcmp(folded,words[i]))return true;
 return false;
}
struct hf_command hf_parse(struct hf_gate *g,const char *text,uint32_t now)
{
 struct hf_command c={HF_IGNORE,0,false,{0}};char normalized[512];
 if(!g||!text||!hf_normalize(text,normalized,sizeof(normalized)))return c;
 const char *t=normalized,*tail=wake_tail(normalized);
 if(tail){c.woke=true;t=tail;hf_keep_awake(g,now);}
 else if(!hf_is_awake(g,now)){g->awake=false;return c;}
 if(filler_only(t)){c.action=c.woke?HF_WAKE:HF_IGNORE;return c;}
 snprintf(c.text,sizeof(c.text),"%s",t);
 c.action=HF_ASK;
 if(!*t)c.action=HF_WAKE;
 else if(IS("停止")||IS("停下")||IS("停一下")||IS("别说了")||IS("别说话")||IS("不要说了")||IS("停止说话")||IS("请停一下")||IS("先停一下")||IS("暂停")||IS("取消")||IS("等一会儿")||IS("等一下")||IS("等一等")||IS("等会儿")||IS("稍等")||IS("不要拍照")||IS("别拍了")||IS("停止拍照")||IS("取消拍照"))c.action=HF_STOP;
 else if(HAS("为什么")||HAS("原理")||HAS("什么意思")||HAS("多久")||HAS("如何")||HAS("怎么浇")||HAS("怎么办")||HAS("要不要")||HAS("怎么养")||HAS("适合")||HAS("应该"))c.action=HF_ASK;
 else if(HAS("不要拍照")||HAS("别拍")||HAS("不想拍照")||HAS("不用拍照")||HAS("取消拍照")||HAS("停止拍照"))c.action=HF_STOP;
 else if(IS("再见")||IS("不用了")||IS("谢谢小伴")){c.action=HF_SLEEP;g->awake=false;}
 /* A negation or a question about a capability must never become an action. */
 else if(HAS("不要")||HAS("不想")||HAS("不需要")||HAS("不用")||HAS("不保存")||HAS("不打开")||HAS("先别")||HAS("别拍")||HAS("别保存")||HAS("不调")||HAS("不能")||HAS("会不会")||HAS("你会")||HAS("怎么拍"))c.action=HF_ASK;
 else if(IS("关掉屏幕")||IS("关闭屏幕")||IS("熄屏"))c.action=HF_SCREEN_OFF;
 else if(IS("省电模式")||IS("开启省电模式"))c.action=HF_POWER_ECO;
 else if(IS("正常模式")||IS("退出省电模式"))c.action=HF_POWER_NORMAL;
 else if(IS("保持亮屏")||IS("屏幕常亮"))c.action=HF_KEEP_SCREEN;
 else if(IS("长时间休息"))c.action=HF_DEEP_REST;
 else if(HAS("休息")){c.action=HF_SLEEP;g->awake=false;}
 else if(HAS("关闭相机")||HAS("关掉相机")||HAS("退出相机")||HAS("关闭摄像头")||HAS("退出拍照"))c.action=HF_CAMERA_CLOSE;
 else if(HAS("取消静音")||HAS("恢复声音")||HAS("打开声音"))c.action=HF_UNMUTE;
 else if(HAS("静音")||IS("安静")||IS("关闭声音"))c.action=HF_MUTE;
 else if(HAS("声音")||HAS("音量")){
  if(HAS("小")||HAS("低")||HAS("轻")){c.action=HF_VOLUME;c.value=-6;}
  else if(HAS("大")||HAS("高")||HAS("响")){c.action=HF_VOLUME;c.value=6;}
  else if((HAS("调到")||HAS("设为")||HAS("设置"))&&number(t)>=0){c.action=HF_VOLUME_SET;c.value=number(t);if(c.value>100)c.value=100;}
 }
 else if(HAS("帮助")||HAS("怎么玩")||HAS("怎么用")||HAS("能做什么"))c.action=HF_HELP;
 else if(HAS("为什么")||HAS("怎么养")||HAS("适合")||HAS("应该"))c.action=HF_ASK;
 /* Confirmed real-microphone ASR homophone. Limit it to a whole short
  * command; negations and questions have already been handled above. */
 else if(IS("回首夜")||HAS("回首页")||HAS("回到首页")||HAS("主页")||HAS("主界面"))c.action=HF_HOME;
 else if(HAS("返回")||IS("回去")||IS("上一步"))c.action=HF_BACK;
 else if(HAS("下一页")||HAS("下一条")||HAS("下一张")||IS("往下")||IS("继续读"))c.action=HF_NEXT;
 else if(IS("上仪也")||IS("上你也")||IS("上疑也")||HAS("上一页")||HAS("上一条")||HAS("上一张")||IS("往上"))c.action=HF_PREVIOUS;
 else if(HAS("相册")||HAS("照片记录"))c.action=HF_DIARY;
 else if(HAS("保存")||HAS("存日记")||HAS("存到日记")||HAS("记下来")||HAS("记一下"))c.action=HF_SAVE;
 else if(IS("再说一遍")||IS("再讲一遍")||IS("没听清")||IS("重复一下"))c.action=HF_REPEAT;
 else if(HAS("识别结果")||HAS("读结果")||HAS("读一下结果")||IS("看看结果")||IS("结果是什么"))c.action=HF_RESULT;
 else if(HAS("这是什么植物")||HAS("这是什么花")||HAS("这是什么树")||HAS("这是啥植物")||HAS("识别一下")||HAS("帮我识别")||HAS("拍照")||HAS("拍一张")||HAS("拍个照")||HAS("照张相")||HAS("照个相")||HAS("重拍")||HAS("识别植物")||HAS("看看植物")||HAS("植物体检"))c.action=HF_PHOTO;
 else if(HAS("摄像头")||HAS("相机")||HAS("取景"))c.action=HF_CAMERA;
 else if(HAS("任务")){
  int n=number(t);
  if(n>0&&(HAS("撤销")||HAS("取消")||HAS("没完成")||HAS("未完成")||HAS("没有完成")||HAS("还没做完"))){c.action=HF_TASK_UNDONE;c.value=n-1;}
  else if(n>0&&(HAS("完成")||HAS("打卡"))){c.action=HF_TASK_DONE;c.value=n-1;}
  else c.action=HF_TASKS;
 }
 else if(HAS("日记")||HAS("成长记录"))c.action=HF_DIARY;
 else if(HAS("网络")||HAS("联网")||HAS("WiFi")||HAS("wifi"))c.action=HF_NETWORK;
 else if(HAS("设备信息")||HAS("固件版本"))c.action=HF_INFO;
 else if(HAS("全部读数")||HAS("所有读数")||HAS("全部数据"))c.action=HF_ALL;
 /* Same-tone ASR substitution observed on the physical BOX. A complete
  * read-only query is safe to repair; unrelated text stays conversational. */
 else if(IS("但元素多少")){c.action=HF_METRIC;c.value=5;}
 else if(IS("钼元素多少")){c.action=HF_HELP;c.value=1;}
 else if(HAS("温度")||HAS("湿度")||HAS("水分")||HAS("酸碱")||HAS("pH")||HAS("ph")||HAS("电导")||HAS("EC")||HAS("ec")||HAS("盐分")||HAS("氮")||HAS("磷")||HAS("钾")){
  c.action=HF_METRIC;
  if(HAS("温度"))c.value=1;
  else if(HAS("电导")||HAS("EC")||HAS("ec"))c.value=2;
  else if(HAS("盐分"))c.value=3;
  else if(HAS("酸碱")||HAS("pH")||HAS("ph"))c.value=4;
  else if(HAS("氮"))c.value=5;else if(HAS("磷"))c.value=6;else if(HAS("钾"))c.value=7;
 }
 else if(element_query(t)){c.action=HF_HELP;c.value=2;}
 else if(HAS("状态")||HAS("数据")||HAS("读数")||HAS("看看小绿"))c.action=HF_DATA;
 else if(HAS("浇水")&&!HAS("需要")&&!HAS("吗"))c.action=HF_WATER;
 else if(HAS("聊天")||HAS("说说话")||IS("说话")||HAS("开始说话")||HAS("和你说话")||HAS("和小伴说话"))c.action=HF_TALK;
 if(c.action!=HF_SLEEP)hf_keep_awake(g,now);
 return c;
}
/* A locally confirmed wake can repair a short name transcription.
 * Preserve recognized commands, including a short command after the wake. */
bool hf_weak_wake_text_matches(const char *text)
{
 char normalized[512];
 return hf_normalize(text,normalized,sizeof(normalized))>0 && wake_tail(normalized)!=NULL;
}
void hf_canonicalize_local_wake(char *text,size_t capacity)
{
 char normalized[512];size_t n=hf_normalize(text,normalized,sizeof(normalized));
 const char *tail=n?wake_tail(normalized):NULL;
 if(tail){snprintf(text,capacity,"你好小伴%s",tail);return;}
 /* Strong sound recognition already woke the device. A short garbled
  * transcription must not turn that wake into an unrelated AI question. */
 if(n && n<=18){
  struct hf_gate g={true,0};struct hf_command c=hf_parse(&g,normalized,0);
  if(c.action==HF_ASK)snprintf(text,capacity,"你好小伴");
 }
}
const char *hf_action_name(enum hf_action a)
{
 static const char *names[]={"ignore","wake","ask","home","data","talk","camera","photo","save","result","tasks","diary","back","next","previous","all","metric","volume","volume_set","mute","unmute","stop","sleep","help","network","info","task_done","task_undone","water","repeat","camera_close","screen_off","power_eco","power_normal","keep_screen","deep_rest"};
 return (unsigned)a<sizeof(names)/sizeof(*names)?names[a]:"invalid";
}
