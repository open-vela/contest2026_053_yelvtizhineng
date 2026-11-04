/****************************************************************************
 * apps/plant-companion/components/voice_agent/aw88298.h
 *
 * Minimal AW88298 I2S speaker amplifier driver for M5Stack CoreS3.
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_AW88298_H
#define __APPS_PLANT_COMPANION_AW88298_H

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>

#ifdef __cplusplus
extern "C"
{
#endif

int aw88298_init(FAR struct i2c_master_s *i2c);
int aw88298_probe(FAR struct i2c_master_s *i2c);
int aw88298_set_format(FAR struct i2c_master_s *i2c, int sample_rate);
int aw88298_start(FAR struct i2c_master_s *i2c);
int aw88298_standby(FAR struct i2c_master_s *i2c,bool standby);
int aw88298_set_volume(FAR struct i2c_master_s *i2c, uint8_t vol);
int aw88298_mute(FAR struct i2c_master_s *i2c, bool mute);
int aw88298_dump_regs(FAR struct i2c_master_s *i2c);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_PLANT_COMPANION_AW88298_H */
