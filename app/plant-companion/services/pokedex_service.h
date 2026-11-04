/****************************************************************************
 * apps/plant-companion/services/pokedex_service.h
 *
 * 植物图鉴（本地缓存）—— 相机「图鉴模式」秒回的数据源。
 *
 * 背景：拍照识别全链路（上传 + 服务器 AI）要 10-40s，小朋友没耐心等。
 * 思路：第一次成功识别后把结果存进 SD（/mnt/sd/pokedex.txt），下次按
 * 快门的瞬间先用图鉴里的名字给出「是绿萝呀！」式秒回；AI 体检仍在后台
 * 照跑，结果回来再上诊断页（健康状态是动态的，以服务器最新结果为准）。
 *
 * 存储格式（行式 key=value，手写读写，不引 JSON 解析器）：
 *   name=绿萝
 *   latin=Epipremnum aureum
 *   health=87
 *   summary=状态不错，继续加油
 * 文本里的换行在写入时替换为空格。
 ****************************************************************************/

#ifndef __PLANT_SERVICES_POKEDEX_SERVICE_H
#define __PLANT_SERVICES_POKEDEX_SERVICE_H

#include <stdbool.h>

#include "server_bridge.h"

struct pokedex_entry_s
{
  bool valid;                 /* 图鉴里有有效条目 */
  char name[64];              /* 植物俗名（UTF-8） */
  char latin[96];             /* 拉丁名（可为空） */
  int  health_score;          /* 上次体检分（<0=未知） */
  char summary[288];          /* 上次一句话总结 */
};

/* 从 SD 加载图鉴到 RAM 缓存（幂等；进相机页时调一次即可）。 */

void pokedex_preload(void);

/* 取图鉴条目。返回 0 = 有有效条目；负值 = 图鉴为空。 */

int pokedex_get(struct pokedex_entry_s *out);

/* 用一次成功的服务器诊断更新图鉴（RAM 缓存 + SD 落盘）。
 * 在 worker 线程调用安全（只做文件写，不碰 LVGL）。 */

void pokedex_update(const struct sb_diag_result_s *diag);

#endif /* __PLANT_SERVICES_POKEDEX_SERVICE_H */
