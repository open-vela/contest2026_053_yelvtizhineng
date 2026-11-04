/****************************************************************************
 * apps/plant-companion/hal/hal_camera.h
 *
 * Hardware abstraction for the camera capture path.
 *
 * Capture frames remain valid until the next capture or stop. Acquired
 * leases remain valid until release; stop/close require all leases returned.
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_HAL_HAL_CAMERA_H
#define __APPS_PLANT_COMPANION_HAL_HAL_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hal_camera_pixfmt_e
{
  HAL_CAMERA_PIXFMT_RGB565 = 0,
} hal_camera_pixfmt_t;

typedef struct hal_camera_config_s
{
  uint16_t width;
  uint16_t height;
  hal_camera_pixfmt_t pixfmt;
  uint32_t flags;
  bool swap_bytes;
} hal_camera_config_t;

#define HAL_CAMERA_CONFIG_PLAN_A  (1u << 0)

typedef struct hal_camera_frame_s
{
  const uint8_t *data;
  size_t length;
  uint32_t sequence;
  uint32_t generation;
  uint32_t flags;
} hal_camera_frame_t;

#define HAL_CAMERA_FRAME_COMPLETE  (1u << 0)

typedef struct hal_camera_lease_s
{
  const uint8_t *data;
  size_t length;
  uint8_t slot;
  uint32_t sequence;
  uint32_t generation;
  bool valid;
} hal_camera_lease_t;

typedef struct hal_camera_health_s
{
  uint32_t sequence;
  uint32_t frame_bytes;
  uint32_t short_frames;
  uint32_t short_a;
  uint32_t short_b;
  uint32_t stale_frames;
  uint32_t sequence_gaps;
  uint32_t drain_timeouts;
  uint32_t last_drain_bytes;
  uint32_t last_wait_us;
  uint32_t last_copy_us;
  uint32_t node_count;
  uint32_t node_partial_frames;
  uint32_t node_frames;
  uint32_t lease_acquires;
  uint32_t lease_releases;
  uint32_t lease_errors;
  uint32_t lease_timeouts;
  uint32_t done_overflow;
  uint8_t lease_held_mask;
} hal_camera_health_t;

int hal_camera_open(const hal_camera_config_t *config);
int hal_camera_start(void);
int hal_camera_stop(void);
void hal_camera_close(void);

int hal_camera_capture(hal_camera_frame_t *frame, uint32_t timeout_ms);
int hal_camera_acquire(hal_camera_lease_t *lease, uint32_t timeout_ms);
int hal_camera_release(hal_camera_lease_t *lease);
int hal_camera_get_health(hal_camera_health_t *health);

#endif /* __APPS_PLANT_COMPANION_HAL_HAL_CAMERA_H */
