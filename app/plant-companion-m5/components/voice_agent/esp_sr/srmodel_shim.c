/****************************************************************************
 * srmodel_shim.c — esp_sr 模型注册表（srmodel 机制）的 NuttX 内存版
 *
 * 原版 model_path.c 依赖 ESP-IDF 分区/SPIFFS/mmap。本文件实现其
 * "srmodel_load(内存)" 路径：模型二进制（srmodels.bin 打包格式，
 * pack_model.py 生成）以 const 数组嵌入固件，解析后 model_data 的
 * data 指针直接指向固件内嵌数据 —— model_create 据此加载模型，
 * 全程无需文件系统。
 *
 * 打包格式（与 pack_model.py / model_path.c 的 srmodel_load 对应）：
 *   int32  model_num
 *   每个模型：
 *     char[32] model_name
 *     int32  file_num
 *     每个文件：char[32] file_name, int32 data_offset, int32 data_size
 *   之后拼接全部文件数据
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model_data/srmodels_data.h"

/****************************************************************************
 * 数据结构（与 esp_sr model_path.h 一致）
 ****************************************************************************/

#define SRMODEL_STRING_LENGTH 32
#define MODEL_NAME_MAX_LENGTH 64

typedef struct
{
  int num;       /* 文件数 */
  char **files;  /* 文件名数组 */
  char **data;   /* 数据指针数组 */
  int *sizes;    /* 文件大小数组 */
} srmodel_data_t;

/* ⚠️ 布局必须与 esp_sr 库（ESP_PLATFORM 编译）完全一致！
 * model_create 直接按偏移访问：model_name=0, model_info=4,
 * partition=8, mmap_handle=12, num=16, model_data=20。
 * 若漏掉 partition/mmap_handle 占位，num/model_data 偏移错位 →
 * model_create 读到垃圾 → "找不到模型" → create 返回 NULL。 */
typedef struct
{
  char **model_name;          /* 0  */
  char **model_info;          /* 4  */
  void *partition;            /* 8  （ESP_PLATFORM: esp_partition_t*）*/
  void *mmap_handle;          /* 12 */
  int num;                    /* 16 */
  srmodel_data_t **model_data; /* 20 */
} srmodel_list_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static srmodel_list_t *g_static_srmodels = NULL;
static char *g_model_base_path = NULL;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static char *get_model_info(char *data, int size)
{
  char *model_info = NULL;

  while (size > 0)
    {
      if (*data == '#')
        {
          while (*data != '\n' && size > 1)
            {
              data++;
              size--;
            }

          data++;
          size--;
          continue;
        }
      else if (data != NULL && size > 0)
        {
          model_info = (char *)malloc((size + 1) * sizeof(char));
          memcpy(model_info, data, size);
          if (model_info[size - 1] == '\n')
            {
              model_info[size - 1] = '\0';
            }

          model_info[size] = '\0';
          break;
        }
    }

  return model_info;
}

static uint32_t read_int32(char *data)
{
  uint32_t value = 0;

  value |= (uint32_t)data[0] << 0;
  value |= (uint32_t)data[1] << 8;
  value |= (uint32_t)data[2] << 16;
  value |= (uint32_t)data[3] << 24;
  return value;
}

static srmodel_list_t *srmodel_list_alloc(void)
{
  srmodel_list_t *models = (srmodel_list_t *)malloc(sizeof(srmodel_list_t));

  models->model_data = NULL;
  models->model_name = NULL;
  models->model_info = NULL;
  models->partition = NULL;
  models->mmap_handle = NULL;
  models->num = 0;
  return models;
}

/****************************************************************************
 * Public Functions（与 esp_sr model_path.h 同签名）
 ****************************************************************************/

/* 从内嵌二进制加载模型注册表（srmodels.bin 格式）*/
srmodel_list_t *srmodel_load(const void *root)
{
  if (g_static_srmodels == NULL)
    {
      g_static_srmodels = srmodel_list_alloc();
    }

  srmodel_list_t *models = g_static_srmodels;
  char *start = (char *)root;
  char *data = (char *)root;
  int str_len = SRMODEL_STRING_LENGTH;
  int int_len = 4;

  /* 模型数 */
  models->num = (int)read_int32(data);
  data += int_len;

  models->model_data = (srmodel_data_t **)malloc(sizeof(srmodel_data_t *) * models->num);
  models->model_name = (char **)malloc(sizeof(char *) * models->num);
  models->model_info = (char **)malloc(sizeof(char *) * models->num);

  for (int i = 0; i < models->num; i++)
    {
      srmodel_data_t *model_data = (srmodel_data_t *)malloc(sizeof(srmodel_data_t));

      models->model_info[i] = NULL;

      /* 模型名（定长 32）*/
      models->model_name[i] = (char *)malloc((strlen(data) + 1) * sizeof(char));
      strcpy(models->model_name[i], data);
      data += str_len;

      /* 文件数 */
      int file_num = (int)read_int32(data);
      model_data->num = file_num;
      data += int_len;

      model_data->files = (char **)malloc(sizeof(char *) * file_num);
      model_data->data = (char **)malloc(sizeof(void *) * file_num);
      model_data->sizes = (int *)malloc(sizeof(int) * file_num);

      for (int j = 0; j < file_num; j++)
        {
          /* 文件名（定长 32）*/
          model_data->files[j] = data;
          data += str_len;

          /* 数据偏移与大小（相对 root）*/
          int index = (int)read_int32(data);
          data += int_len;

          model_data->data[j] = start + index;

          int size = (int)read_int32(data);
          data += int_len;

          model_data->sizes[j] = size;

          if (strcmp(model_data->files[j], "_MODEL_INFO_") == 0)
            {
              models->model_info[i] = get_model_info(model_data->data[j],
                                                     model_data->sizes[j]);
            }
        }

      models->model_data[i] = model_data;
    }

  /* model_create 判定：base path 为 NULL 时走内存数据路径 */
  g_model_base_path = NULL;

  /* 诊断：验证解析结果（model_create 靠 name/files strcmp 匹配） */
  printf("[srmodel] %d model(s):\n", models->num);
  for (int i = 0; i < models->num; i++)
    {
      printf("  [%d] name=\"%s\" files: ", i, models->model_name[i]);
      for (int j = 0; j < models->model_data[i]->num; j++)
        {
          printf("%s(%dB) ", models->model_data[i]->files[j],
                 models->model_data[i]->sizes[j]);
        }

      printf("\n");
    }

  return models;
}

void set_model_base_path(const char *base_path)
{
  g_model_base_path = (char *)base_path;
}

char *get_model_base_path(void)
{
  return g_model_base_path;
}

srmodel_list_t *get_static_srmodels(void)
{
  return g_static_srmodels;
}

/* 按前缀关键词过滤模型名（用于 AFE/wakenet；NS 场景传 "nsnet"）*/
char *esp_srmodel_filter(srmodel_list_t *models, const char *keyword1,
                         const char *keyword2)
{
  if (models == NULL || models->model_name == NULL)
    {
      return NULL;
    }

  for (int i = 0; i < models->num; i++)
    {
      if (keyword1 != NULL && strstr(models->model_name[i], keyword1) == NULL)
        {
          continue;
        }

      if (keyword2 != NULL && strstr(models->model_name[i], keyword2) == NULL)
        {
          continue;
        }

      return models->model_name[i];
    }

  return NULL;
}

/* 检查模型是否存在，返回索引或 -1 */
int esp_srmodel_exists(srmodel_list_t *models, char *model_name)
{
  if (models == NULL || model_name == NULL)
    {
      return -1;
    }

  for (int i = 0; i < models->num; i++)
    {
      if (strcmp(models->model_name[i], model_name) == 0)
        {
          return i;
        }
    }

  return -1;
}

/****************************************************************************
 * 初始化入口（ai_ns_init 调用）
 ****************************************************************************/

int srmodel_nuttx_init(void)
{
  if (g_static_srmodels != NULL)
    {
      return 0;  /* 已初始化 */
    }

  srmodel_load((const void *)g_srmodels_nsnet2_bin);

  if (g_static_srmodels == NULL || g_static_srmodels->num == 0)
    {
      printf("srmodel: load failed\n");
      return -1;
    }

  printf("srmodel: %d model(s) loaded (%s)\n", g_static_srmodels->num,
         g_static_srmodels->model_name[0]);
  return 0;
}
