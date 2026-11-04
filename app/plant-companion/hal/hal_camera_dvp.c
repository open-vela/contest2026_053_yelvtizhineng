/****************************************************************************
 * apps/plant-companion/hal/hal_camera_dvp.c
 *
 * ESP32-S3 DVP backend for hal_camera.
 *
 * This is the only application-side file that knows the current NuttX
 * esp32s3_cam_dvp API. A future IDF-style backend can replace this
 * implementation without changing camera_capture or UI callers.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_camera.h"

#ifdef CONFIG_ESP32S3_CAM_DVP

extern int esp32s3_cam_dvp_init(void);
extern int esp32s3_cam_dvp_start(void);
extern int esp32s3_cam_dvp_stop(void);
extern int esp32s3_cam_dvp_capture(void (*sink)(const uint8_t *data,
                                                 size_t len, void *arg),
                                   void *sink_arg,
                                   volatile bool *eoi_out);
extern uint8_t *esp32s3_cam_dvp_get_frame(void);
extern void esp32s3_cam_dvp_set_stream(bool on);
extern void esp32s3_cam_dvp_set_stream_direct(bool on);
extern void esp32s3_cam_dvp_set_swap(bool on);
extern int esp32s3_cam_dvp_set_framesize(uint32_t w, uint32_t h);
extern void esp32s3_cam_dvp_set_node_mode(bool on);
extern uint32_t esp32s3_cam_dvp_get_node_count(void);
extern uint32_t esp32s3_cam_dvp_get_node_partial_frames(void);
extern uint32_t esp32s3_cam_dvp_get_node_frames(void);
extern uint32_t esp32s3_cam_dvp_get_frame_seq(void);
extern uint32_t esp32s3_cam_dvp_get_frame_generation(void);
extern uint32_t esp32s3_cam_dvp_get_last_frame_bytes(void);
extern uint32_t esp32s3_cam_dvp_get_short_frames(void);
extern uint32_t esp32s3_cam_dvp_get_short_frames_half(int half);
extern uint32_t esp32s3_cam_dvp_get_stale_frames(void);
extern uint32_t esp32s3_cam_dvp_get_seq_gaps(void);
extern uint32_t esp32s3_cam_dvp_get_drain_timeouts(void);
extern uint32_t esp32s3_cam_dvp_get_last_drain_bytes(void);
extern uint32_t esp32s3_cam_dvp_get_last_wait_us(void);
extern uint32_t esp32s3_cam_dvp_get_last_copy_us(void);

extern int esp32s3_cam_dvp_acquire(uint8_t *slot, uint32_t *generation,
                                   uint32_t *sequence,
                                   const uint8_t **frame, size_t *bytes,
                                   uint32_t timeout_ms);
extern int esp32s3_cam_dvp_release(uint8_t slot);

extern void esp32s3_cam_dvp_get_lease_stats(uint32_t *acquires,
                                           uint32_t *releases,
                                           uint32_t *errors,
                                           uint32_t *timeouts,
                                           uint32_t *overflow,
                                           uint8_t *held_mask);

static bool g_hal_camera_open;
static uint32_t g_hal_camera_frame_bytes;

int hal_camera_open(const hal_camera_config_t *config)
{
  int ret;

  if (config == NULL || config->width == 0 || config->height == 0 ||
      config->pixfmt != HAL_CAMERA_PIXFMT_RGB565)
    {
      return -EINVAL;
    }

  ret = esp32s3_cam_dvp_set_framesize(config->width, config->height);
  if (ret < 0)
    {
      return ret;
    }

  esp32s3_cam_dvp_set_stream(true);
  esp32s3_cam_dvp_set_stream_direct(false);
  esp32s3_cam_dvp_set_swap(config->swap_bytes);
  esp32s3_cam_dvp_set_node_mode(
    (config->flags & HAL_CAMERA_CONFIG_PLAN_A) != 0);

  ret = esp32s3_cam_dvp_init();
  if (ret < 0)
    {
      return ret;
    }

  g_hal_camera_frame_bytes =
    (uint32_t)config->width * (uint32_t)config->height * 2u;
  g_hal_camera_open = true;
  return 0;
}

int hal_camera_start(void)
{
  if (!g_hal_camera_open)
    {
      return -EINVAL;
    }

  return esp32s3_cam_dvp_start();
}

int hal_camera_stop(void)
{
  esp32s3_cam_dvp_stop();
  return 0;
}

void hal_camera_close(void)
{
  esp32s3_cam_dvp_stop();
  esp32s3_cam_dvp_set_node_mode(false);
  esp32s3_cam_dvp_set_stream(false);
  esp32s3_cam_dvp_set_stream_direct(false);
  g_hal_camera_open = false;
}

int hal_camera_capture(hal_camera_frame_t *frame, uint32_t timeout_ms)
{
  const uint8_t *data;
  hal_camera_health_t health;
  volatile bool eoi = false;
  uint32_t attempts = 0;
  uint32_t max_attempts;
  int ret;

  if (!g_hal_camera_open || frame == NULL)
    {
      return -EINVAL;
    }

  max_attempts = timeout_ms > 0 ? timeout_ms / 100u : 30u;
  if (max_attempts == 0)
    {
      max_attempts = 1;
    }

  for (;;)
    {
      ret = esp32s3_cam_dvp_capture(NULL, NULL, &eoi);
      if (ret < 0)
        {
          return ret;
        }

      if (hal_camera_get_health(&health) < 0)
        {
          return -EIO;
        }

      if (health.last_drain_bytes == g_hal_camera_frame_bytes)
        {
          break;
        }

      if (++attempts >= max_attempts)
        {
          return -EAGAIN;
        }
    }

  data = esp32s3_cam_dvp_get_frame();
  if (data == NULL)
    {
      return -EFAULT;
    }

  frame->data = data;
  frame->length = (size_t)ret;
  frame->sequence = esp32s3_cam_dvp_get_frame_seq();
  frame->generation = esp32s3_cam_dvp_get_frame_generation();
  frame->flags = ((uint32_t)ret == g_hal_camera_frame_bytes) ?
                 HAL_CAMERA_FRAME_COMPLETE : 0;
  return 0;
}

int hal_camera_acquire(hal_camera_lease_t *lease, uint32_t timeout_ms)
{
  const uint8_t *data;
  size_t bytes;
  uint32_t generation;
  uint32_t sequence;
  uint8_t slot;
  int ret;

  if (!g_hal_camera_open || lease == NULL)
    {
      return -EINVAL;
    }

  lease->valid = false;
  ret = esp32s3_cam_dvp_acquire(&slot, &generation, &sequence, &data,
                                &bytes, timeout_ms);
  if (ret < 0)
    {
      return ret;
    }

  lease->data = data;
  lease->length = bytes;
  lease->slot = slot;
  lease->generation = generation;
  lease->sequence = sequence;
  lease->valid = true;
  return 0;
}

int hal_camera_release(hal_camera_lease_t *lease)
{
  int ret;

  if (lease == NULL || !lease->valid)
    {
      return -EINVAL;
    }

  ret = esp32s3_cam_dvp_release(lease->slot);
  if (ret == 0)
    {
      lease->valid = false;
    }

  return ret;
}

int hal_camera_get_health(hal_camera_health_t *health)
{
  if (health == NULL)
    {
      return -EINVAL;
    }

  health->sequence = esp32s3_cam_dvp_get_frame_seq();
  health->frame_bytes = esp32s3_cam_dvp_get_last_frame_bytes();
  health->short_frames = esp32s3_cam_dvp_get_short_frames();
  health->short_a = esp32s3_cam_dvp_get_short_frames_half(0);
  health->short_b = esp32s3_cam_dvp_get_short_frames_half(1);
  health->stale_frames = esp32s3_cam_dvp_get_stale_frames();
  health->sequence_gaps = esp32s3_cam_dvp_get_seq_gaps();
  health->drain_timeouts = esp32s3_cam_dvp_get_drain_timeouts();
  health->last_drain_bytes = esp32s3_cam_dvp_get_last_drain_bytes();
  health->last_wait_us = esp32s3_cam_dvp_get_last_wait_us();
  health->last_copy_us = esp32s3_cam_dvp_get_last_copy_us();
  health->node_count = esp32s3_cam_dvp_get_node_count();
  health->node_partial_frames = esp32s3_cam_dvp_get_node_partial_frames();
  health->node_frames = esp32s3_cam_dvp_get_node_frames();
  esp32s3_cam_dvp_get_lease_stats(&health->lease_acquires,
                                  &health->lease_releases,
                                  &health->lease_errors,
                                  &health->lease_timeouts,
                                  &health->done_overflow,
                                  &health->lease_held_mask);
  return 0;
}

#else

int hal_camera_open(const hal_camera_config_t *config)
{
  (void)config;
  return -ENOSYS;
}

int hal_camera_start(void)
{
  return -ENOSYS;
}

int hal_camera_stop(void)
{
  return -ENOSYS;
}

void hal_camera_close(void)
{
}

int hal_camera_capture(hal_camera_frame_t *frame, uint32_t timeout_ms)
{
  (void)frame;
  (void)timeout_ms;
  return -ENOSYS;
}

int hal_camera_acquire(hal_camera_lease_t *lease, uint32_t timeout_ms)
{
  (void)lease;
  (void)timeout_ms;
  return -ENOSYS;
}

int hal_camera_release(hal_camera_lease_t *lease)
{
  (void)lease;
  return -ENOSYS;
}

int hal_camera_get_health(hal_camera_health_t *health)
{
  (void)health;
  return -ENOSYS;
}

#endif /* CONFIG_ESP32S3_CAM_DVP */
