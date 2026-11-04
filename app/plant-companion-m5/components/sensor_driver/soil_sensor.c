/* Cloud-only soil cache adapter. No UART, Modbus or GPIO ownership. */
#include <nuttx/config.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include "soil_sensor.h"
#include "../../services/sensor_service.h"

int soil_sensor_probe(int addr,int baudrate,uint8_t *resp,int resp_size)
{
  (void)addr;(void)baudrate;(void)resp;(void)resp_size;return -ENOTSUP;
}
int soil_sensor_frame_valid(const uint8_t *resp,int len,uint8_t addr)
{(void)resp;(void)len;(void)addr;return 0;}
void soil_sensor_set_paused(bool paused)
{(void)paused; /* The shared worker owns its lifecycle; cached reads do no I/O. */}
int soil_sensor_init(const char *dev,int baudrate)
{
  (void)dev;(void)baudrate;
  printf("[SOIL] source=cloud local_uart=disabled\n");
  return 0;
}

void soil_sensor_deinit(void)
{
  /* fd 每次事务自开自关，这里没有常驻资源要释放 */
}

int soil_sensor_read(struct soil_data_s *data,int verbose)
{
  if(!data)return -EINVAL;
  struct sensor_view_s v;struct timespec ts;
  sensor_service_get_view(&v);
  if(clock_gettime(CLOCK_MONOTONIC,&ts)<0)return -EIO;
  uint32_t now=(uint32_t)(ts.tv_sec*1000u+ts.tv_nsec/1000000u);
  if(!v.valid || !v.last_read_ms || (uint32_t)(now-v.last_read_ms)>125000u)return -EAGAIN;
  data->temp=v.temp;data->moisture=v.moisture;data->ec=v.ec;data->salt=v.salt;
  data->nitrogen=v.nitrogen;data->phosphorus=v.phosphorus;data->potassium=v.potassium;data->ph=v.ph;
  if(verbose)printf("[SOIL] source=cloud cached=1 age_ms=%u local_uart=disabled\n",(uint32_t)(now-v.last_read_ms));
  return 0;
}

void soil_sensor_print_data(const struct soil_data_s *data)
{
  if (!data)
    {
      return;
    }

  printf("[SOIL] ====== Soil Sensor Data ======\n");
  printf("[SOIL] Temp:     %.1f C\n", data->temp);
  printf("[SOIL] Moisture: %.1f %%\n", data->moisture);
  printf("[SOIL] EC:       %.0f uS/cm\n", data->ec);
  printf("[SOIL] Salt:     %.0f\n", data->salt);
  printf("[SOIL] N:        %.0f mg/kg\n", data->nitrogen);
  printf("[SOIL] P:        %.0f mg/kg\n", data->phosphorus);
  printf("[SOIL] K:        %.0f mg/kg\n", data->potassium);
  printf("[SOIL] pH:       %.1f\n", data->ph);
  printf("[SOIL] ==============================\n");
}
