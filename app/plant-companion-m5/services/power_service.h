#ifndef PLANT_POWER_SERVICE_H
#define PLANT_POWER_SERVICE_H
#include <stdbool.h>
void power_service_init(void); /* UI task, after display and services start */
void power_service_tick(bool busy); /* UI task only */
void power_service_activity(void); /* thread safe, explicit user activity */
bool power_service_standby(void);
bool power_service_background_allowed(void);
void power_service_menu(void); /* thread-safe menu request */
int power_service_request(const char *mode);
int power_service_command(int argc,char **argv);
#endif
