/****************************************************************************
 * apps/plant-companion/services/ai_service.h
 *
 * AI 业务服务：ai_engine 的异步封装
 *
 * 设计（ARCHITECTURE.md §3.2）：
 *   - ai_engine_run 是阻塞 HTTP 调用（15-60s），不能在 ui_task 里跑
 *   - 本服务用后台线程执行，完成后通过回调把结果交给 UI
 *   - 回调在 ai 工作线程触发；UI 端必须投递到 ui_task 再操作 LVGL
 ****************************************************************************/

#ifndef __PLANT_SERVICES_AI_SERVICE_H
#define __PLANT_SERVICES_AI_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#include "../ai_module/ai_engine/ai_engine.h"
#include "server_bridge.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 请求类型 */

#define AI_REQ_DIAGNOSE    0   /* 图像识别 + 诊断（拍照） */
#define AI_REQ_ADVISE      1   /* 传感器文本建议（浇水/施肥） */

/* 完成回调（ai 工作线程触发） */

struct ai_service_result_s
{
  int  req_type;
  bool success;
  bool from_server;                /* 结果为服务器真实识别（非本地降级） */
  int  server_err;                 /* 服务器链路错误码（0=服务器已应答） */
  char server_text[1024];             /* 3C-2：服务器(MiMo)诊断文本 */
  struct sb_diag_result_s diag;       /* 协议 v2：结构化诊断（拍照页用） */
  struct ai_engine_result_s engine;   /* 完整结果（本地 fallback） */
};

typedef void (*ai_service_done_cb_t)(void *user_data,
                                     const struct ai_service_result_s *result);

/**
 * 启动 AI 服务（初始化 ai_engine；懒加载，可多次调用）
 * @return 0 成功；负值失败
 */
int ai_service_init(void);

/** 停止并释放 */
void ai_service_deinit(void);

/**
 * 提交一次异步诊断请求（拍照识别 + 土壤建议）
 * @param jpeg_data 图像数据（可 NULL：仅文本建议）
 * @param jpeg_len  图像长度
 * @param borrowed  true=零拷贝借用（调用方保证存活，worker 不 free）；
 *                  false=内部拷贝一份
 * @param soil      土壤数据（可 NULL）
 * @param cb        完成回调（可 NULL）
 * @param user_data 透传
 * @return 0 已提交；负值失败（如仍在处理中）
 */
int ai_service_analyze_async(const uint8_t *jpeg_data, size_t jpeg_len,
                             bool borrowed,
                             const struct soil_data_s *soil,
                             ai_service_done_cb_t cb, void *user_data);

/** 是否正在处理（避免重复提交） */
bool ai_service_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_SERVICES_AI_SERVICE_H */
