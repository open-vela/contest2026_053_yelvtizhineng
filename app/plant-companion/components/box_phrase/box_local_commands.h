#ifndef BOX_LOCAL_COMMANDS_H
#define BOX_LOCAL_COMMANDS_H
#include <stddef.h>
/* One acoustic ID accepts several natural expressions. ID 70 is a
 * non-executable contrast class: negations/questions must go to ASR. */
struct box_local_phrase {int id;const char *text,*phonemes;};
static const struct box_local_phrase box_local_phrases[]={
 {40,"打开相机","da kai xiang ji"},{40,"打开相机","qing bang wo da kai xiang ji"},
 {40,"打开相机","da kai she xiang tou"},
 {41,"帮我拍照","bang wo pai zhao"},{41,"帮我拍照","pai zhao"},
 {41,"帮我拍照","pai yi zhang"},{41,"帮我拍照","pai ge zhao"},
 {41,"帮我拍照","zhi wu ti jian"},{41,"帮我拍照","gei zhi wu zuo ge ti jian"},
 {42,"回到首页","hui dao shou ye"},{42,"回到首页","hui shou ye"},
 {42,"回到首页","hui dao zhu jie mian"},
 {43,"看看数据","kan kan shu ju"},{43,"看看数据","cha kan tu rang shu ju"},
 {43,"看看数据","kan kan chuan gan qi de shu ju"},{43,"看看数据","cha kan zhuang tai"},
 {43,"看看数据","kan kan tu rang zhuang tai"},{43,"看看数据","kan kan xiao lv de zhuang tai"},
 {43,"看看数据","cha kan shu ju"},{43,"看看数据","kan kan shu ju ba"},
 {44,"土壤水分是多少","tu rang shi du shi duo shao"},
 {44,"土壤水分是多少","tu rang shui fen shi duo shao"},
 {45,"土壤温度是多少","tu rang wen du shi duo shao"},
 {46,"全部读数","quan bu du shu"},{46,"全部读数","cha kan quan bu shu ju"},
 {47,"打开成长日记","da kai cheng zhang ri ji"},
 {47,"打开成长日记","da kai xiang ce"},{47,"打开成长日记","qing bang wo da kai xiang ce"},
 {48,"返回","fan hui"},{48,"返回","hui dao shang yi ye"},
 {49,"保存到成长日记","bao cun dao cheng zhang ri ji"},
 {49,"保存到成长日记","bao cun zhao pian"},{49,"保存到成长日记","bao cun jie guo"},
 {50,"休息","xiu xi"},{50,"休息","xiu xi ba"},{50,"休息","xie xie xiao ban"},
 {50,"休息","xiao ban xiu xi ba"},{50,"休息","shui jiao ba"},
 {51,"关闭相机","guan bi xiang ji"},{51,"关闭相机","tui chu xiang ji"},
 {52,"读一下结果","du yi xia jie guo"},{52,"读一下结果","shi bie jie guo"},
 {53,"关闭屏幕","guan bi ping mu"},{53,"关闭屏幕","xi ping"},
 {70,NULL,"bu yao"},{70,NULL,"bu yong"},{70,NULL,"qu xiao"},
 {70,NULL,"bu yao pai zhao"},{70,NULL,"bie pai zhao"},
 {70,NULL,"bu yao da kai xiang ji"},{70,NULL,"bu yao bao cun"},
 {70,NULL,"ni hui pai zhao ma"},{70,NULL,"zen me pai zhao"},
 {70,NULL,"zen me da kai xiang ji"},{70,NULL,"wei shen me"},{70,NULL,"ru he"}
};
static inline const char *box_local_command_text(int id)
{
 if(id==2)return "停一下";
 for(size_t i=0;i<sizeof(box_local_phrases)/sizeof(box_local_phrases[0]);i++)
  if(box_local_phrases[i].id==id)return box_local_phrases[i].text;
 return NULL;
}
static inline int box_local_command_blocked(int id)
{return id==70||(id>=20&&id<=28);}
#endif
