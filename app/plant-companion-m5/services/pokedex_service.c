/****************************************************************************
 * apps/plant-companion/services/pokedex_service.c
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "pokedex_service.h"

#define POKEDEX_PATH  "/mnt/sd/pokedex.txt"

static struct pokedex_entry_s s_entry;
static bool s_loaded;

/* 文本里的换行替换为空格（行式存储不支持多行值） */

static void flatten(char *s)
{
  for (; *s != '\0'; s++)
    {
      if (*s == '\n' || *s == '\r')
        {
          *s = ' ';
        }
    }
}

void pokedex_preload(void)
{
  FILE *fp;
  char line[320];

  if (s_loaded)
    {
      return;
    }

  s_loaded = true;
  memset(&s_entry, 0, sizeof(s_entry));
  s_entry.health_score = -1;

  fp = fopen(POKEDEX_PATH, "r");
  if (fp == NULL)
    {
      printf("[Pokedex] 图鉴为空（首次使用）\n");
      return;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      line[strcspn(line, "\r\n")] = '\0';

      if (strncmp(line, "name=", 5) == 0)
        {
          strlcpy(s_entry.name, line + 5, sizeof(s_entry.name));
        }
      else if (strncmp(line, "latin=", 6) == 0)
        {
          strlcpy(s_entry.latin, line + 6, sizeof(s_entry.latin));
        }
      else if (strncmp(line, "health=", 7) == 0)
        {
          s_entry.health_score = atoi(line + 7);
        }
      else if (strncmp(line, "summary=", 8) == 0)
        {
          strlcpy(s_entry.summary, line + 8, sizeof(s_entry.summary));
        }
    }

  fclose(fp);
  s_entry.valid = (s_entry.name[0] != '\0');
  printf("[Pokedex] 图鉴已加载: %s\n",
         s_entry.valid ? s_entry.name : "(空)");
}

int pokedex_get(struct pokedex_entry_s *out)
{
  pokedex_preload();

  if (!s_entry.valid)
    {
      return -ENOENT;
    }

  if (out != NULL)
    {
      *out = s_entry;
    }

  return 0;
}

/* 不可信识别结果的名称特征：认不出 / 报错文案，绝不能进图鉴。
 * v188：旧过滤只拦"未知"，漏了"无法识别/未识别植物"（实测 match=10
 * 的"无法识别"差点写进图鉴）。 */

static bool pokedex_name_bad(const char *name)
{
  static const char *bad[] =
  {
    "未知", "未识别", "无法识别", "未能识别", "识别失败",
    "没有看到", "没有植物", "无效", "error", "Error", NULL
  };
  int i;

  for (i = 0; bad[i] != NULL; i++)
    {
      if (strstr(name, bad[i]) != NULL)
        {
          return true;
        }
    }

  return false;
}

void pokedex_update(const struct sb_diag_result_s *diag)
{
  /* 用静态缓冲：调用线程栈可能很小，fopen/FILE + 400B 局部数组
   * 实测会把 fopen 打成 EINVAL（落盘失败 errno=22）。 */

  static char s_line[768];
  char name[64];
  char latin[96];
  char summary[288];
  int n;
  int fd;

  if (diag == NULL || diag->name[0] == '\0' ||
      diag->match <= 0 || pokedex_name_bad(diag->name))
    {
      printf("[Pokedex] 结果不可信（%s/匹配%d），不入图鉴\n",
             diag != NULL ? diag->name : "(null)",
             diag != NULL ? diag->match : -1);
      return;
    }

  strlcpy(name, diag->name, sizeof(name));
  strlcpy(latin, diag->latin, sizeof(latin));
  strlcpy(summary, diag->summary, sizeof(summary));
  flatten(name);
  flatten(latin);
  flatten(summary);

  pokedex_preload();

  strlcpy(s_entry.name, name, sizeof(s_entry.name));
  strlcpy(s_entry.latin, latin, sizeof(s_entry.latin));
  strlcpy(s_entry.summary, summary, sizeof(s_entry.summary));
  s_entry.health_score = diag->health_score;
  s_entry.valid = true;

  n = snprintf(s_line, sizeof(s_line),
               "name=%s\nlatin=%s\nhealth=%d\nsummary=%s\n",
               s_entry.name, s_entry.latin, s_entry.health_score,
               s_entry.summary);
  if (n < 0)
    {
      printf("[Pokedex] 格式化失败（仅内存生效）\n");
      return;
    }

  if (n >= (int)sizeof(s_line))
    {
      n = (int)sizeof(s_line) - 1;
    }

  /* v188：改用 open/write/close。fopen("w") 在本路径实测 errno=22，
   * 同一时刻 echo/SD 命令却能写 → FILE* 路径不稳，落到 sys_call。 */

  fd = open(POKEDEX_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      printf("[Pokedex] 落盘失败: open errno=%d（仅内存生效）\n",
             errno);
      return;
    }

  if (write(fd, s_line, (size_t)n) != (ssize_t)n)
    {
      printf("[Pokedex] 落盘失败: write errno=%d（仅内存生效）\n",
             errno);
      close(fd);
      return;
    }

  close(fd);
  printf("[Pokedex] 图鉴已更新: %s（健康 %d）\n",
         s_entry.name, s_entry.health_score);
}
