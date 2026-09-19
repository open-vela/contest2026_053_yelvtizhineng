/****************************************************************************
 * sensor_driver.h — Modbus RTU 土壤传感器
 *
 * Phase 16: RS485 soil probe (moisture/temp/humidity/EC/pH/NPK)
 ****************************************************************************/
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int soil_sensor_init(void);
int soil_sensor_read(float *moisture, float *temp, float *ec, float *ph);
#ifdef __cplusplus
}
#endif
