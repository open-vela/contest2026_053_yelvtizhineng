#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "soil_app.h"
#include "soil_state.h"

#define SOIL_STATE_PATH "/data/soil_state.cfg"
#define SOIL_STATE_MAGIC "SOILSTATE1"

static struct soil_state_s g_state;
static bool g_loaded;

static void state_defaults(void)
{
  memset(&g_state, 0, sizeof(g_state));
}

static int state_load(void)
{
  FILE *file;
  char line[160];
  bool magic_ok = false;

  state_defaults();
  file = fopen(SOIL_STATE_PATH, "r");
  if (file == NULL)
    {
      g_loaded = true;
      return -errno;
    }

  while (fgets(line, sizeof(line), file) != NULL)
    {
      char *value;
      size_t length = strlen(line);

      while (length > 0 && (line[length - 1] == '\n' ||
                            line[length - 1] == '\r'))
        {
          line[--length] = '\0';
        }

      value = strchr(line, '=');
      if (value == NULL)
        {
          continue;
        }

      *value++ = '\0';
      if (strcmp(line, "magic") == 0)
        {
          magic_ok = strcmp(value, SOIL_STATE_MAGIC) == 0;
        }
      else if (strcmp(line, "boot_count") == 0)
        {
          g_state.boot_count = strtoul(value, NULL, 10);
        }
      else if (strcmp(line, "last_update") == 0)
        {
          g_state.last_update = strtoul(value, NULL, 10);
        }
      else if (strcmp(line, "last_rssi") == 0)
        {
          g_state.last_rssi = atoi(value);
        }
      else if (strcmp(line, "sample_ok") == 0)
        {
          g_state.last_sample_ok = atoi(value) != 0;
        }
      else if (strcmp(line, "publish_ok") == 0)
        {
          g_state.last_publish_ok = atoi(value) != 0;
        }
      else if (strcmp(line, "has_reading") == 0)
        {
          g_state.has_reading = atoi(value) != 0;
        }
      else if (strcmp(line, "temp_x10") == 0)
        {
          g_state.last_reading.temp_x10 = (int16_t)atoi(value);
        }
      else if (strcmp(line, "moisture_x10") == 0)
        {
          g_state.last_reading.moisture_x10 = (uint16_t)atoi(value);
        }
      else if (strcmp(line, "ec") == 0)
        {
          g_state.last_reading.ec = (uint16_t)atoi(value);
        }
      else if (strcmp(line, "ph_x10") == 0)
        {
          g_state.last_reading.ph_x10 = (uint16_t)atoi(value);
        }
      else if (strcmp(line, "salt") == 0)
        {
          g_state.last_reading.salt = (uint16_t)atoi(value);
        }
      else if (strcmp(line, "nitrogen") == 0)
        {
          g_state.last_reading.nitrogen = (uint16_t)atoi(value);
        }
      else if (strcmp(line, "phosphorus") == 0)
        {
          g_state.last_reading.phosphorus = (uint16_t)atoi(value);
        }
      else if (strcmp(line, "potassium") == 0)
        {
          g_state.last_reading.potassium = (uint16_t)atoi(value);
        }
    }

  fclose(file);
  g_loaded = true;
  return magic_ok ? 0 : -EINVAL;
}

static int state_save(void)
{
  FILE *file;
  char temp_path[] = SOIL_STATE_PATH ".tmp";

  file = fopen(temp_path, "w");
  if (file == NULL)
    {
      return -errno;
    }

  fprintf(file, "magic=%s\n", SOIL_STATE_MAGIC);
  fprintf(file, "boot_count=%lu\n", (unsigned long)g_state.boot_count);
  fprintf(file, "last_update=%lu\n", (unsigned long)g_state.last_update);
  fprintf(file, "last_rssi=%d\n", g_state.last_rssi);
  fprintf(file, "sample_ok=%u\n", g_state.last_sample_ok ? 1 : 0);
  fprintf(file, "publish_ok=%u\n", g_state.last_publish_ok ? 1 : 0);
  fprintf(file, "has_reading=%u\n", g_state.has_reading ? 1 : 0);
  fprintf(file, "temp_x10=%d\n", g_state.last_reading.temp_x10);
  fprintf(file, "moisture_x10=%u\n", g_state.last_reading.moisture_x10);
  fprintf(file, "ec=%u\n", g_state.last_reading.ec);
  fprintf(file, "ph_x10=%u\n", g_state.last_reading.ph_x10);
  fprintf(file, "salt=%u\n", g_state.last_reading.salt);
  fprintf(file, "nitrogen=%u\n", g_state.last_reading.nitrogen);
  fprintf(file, "phosphorus=%u\n", g_state.last_reading.phosphorus);
  fprintf(file, "potassium=%u\n", g_state.last_reading.potassium);

  if (fflush(file) != 0 || fsync(fileno(file)) != 0)
    {
      int ret = -errno;
      fclose(file);
      unlink(temp_path);
      return ret;
    }

  fclose(file);
  if (rename(temp_path, SOIL_STATE_PATH) != 0)
    {
      int ret = -errno;
      unlink(temp_path);
      return ret;
    }

  return 0;
}

int soil_state_init(void)
{
  if (!g_loaded)
    {
      state_load();
    }

  g_state.boot_count++;
  return 0;
}

void soil_state_record(const struct soil_reading_s *reading,
                       bool sample_ok, bool publish_ok, int rssi)
{
  int ret;

  g_state.last_sample_ok = sample_ok;
  g_state.last_publish_ok = publish_ok;
  g_state.last_rssi = rssi;
  g_state.last_update = (uint32_t)time(NULL);
  if (reading != NULL)
    {
      g_state.last_reading = *reading;
      g_state.has_reading = true;
    }

  ret = state_save();
  soil_log("STATE_SAVE=%s ret=%d boot_count=%lu\n",
           ret == 0 ? "OK" : "FAIL", ret,
           (unsigned long)g_state.boot_count);
}

int soil_state_json(char *buffer, size_t buffer_size)
{
  int written;

  if (!g_loaded)
    {
      state_load();
    }

  written = snprintf(
    buffer, buffer_size,
    "{\"boot_count\":%lu,\"last_update\":%lu,\"rssi\":%d,"
    "\"sample_ok\":%u,\"publish_ok\":%u,\"has_reading\":%u,"
    "\"last_reading\":{\"moisture\":%.1f,\"temp\":%.1f,\"ec\":%u,"
    "\"ph\":%.1f,\"salt\":%u,\"nitrogen\":%u,\"phosphorus\":%u,"
    "\"potassium\":%u}}",
    (unsigned long)g_state.boot_count,
    (unsigned long)g_state.last_update,
    g_state.last_rssi,
    g_state.last_sample_ok ? 1 : 0,
    g_state.last_publish_ok ? 1 : 0,
    g_state.has_reading ? 1 : 0,
    g_state.last_reading.moisture_x10 / 10.0,
    g_state.last_reading.temp_x10 / 10.0,
    g_state.last_reading.ec,
    g_state.last_reading.ph_x10 / 10.0,
    g_state.last_reading.salt,
    g_state.last_reading.nitrogen,
    g_state.last_reading.phosphorus,
    g_state.last_reading.potassium);

  return written < 0 ? -EIO : 0;
}
