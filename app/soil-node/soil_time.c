#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <netutils/ntpclient.h>

#include "soil_app.h"
#include "soil_time.h"

#define SOIL_VALID_TIME_MIN 1767225600LL

static bool soil_time_is_valid(void)
{
  time_t now = time(NULL);

  return now >= SOIL_VALID_TIME_MIN;
}

int soil_time_sync(uint32_t timeout_ms)
{
  uint32_t waited_ms = 0;
  int ret;

  if (soil_time_is_valid())
    {
      soil_log("TIME_VALID epoch=%lld\n", (long long)time(NULL));
      return 0;
    }

  ret = ntpc_start();
  if (ret < 0 && ret != -EEXIST)
    {
      soil_log("TIME_SYNC_START_FAILED ret=%d\n", ret);
      return ret;
    }

  while (waited_ms < timeout_ms)
    {
      if (soil_time_is_valid())
        {
          soil_log("TIME_SYNC_OK epoch=%lld\n", (long long)time(NULL));
          return 0;
        }

      usleep(200000);
      waited_ms += 200;
    }

  soil_log("TIME_SYNC_TIMEOUT epoch=%lld\n", (long long)time(NULL));
  return -ETIMEDOUT;
}
