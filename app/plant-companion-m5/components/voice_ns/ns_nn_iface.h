#pragma once
#include <stdint.h>
#include <stddef.h>
/* Exact SDK2.3.1 NSnet2 DWARF interface; model state stays opaque. */
typedef struct esp_nsn_data_t plant_nn_data_t;
struct plant_nn_iface
{
  plant_nn_data_t *(*create)(char *model_name);
  int (*get_samp_chunksize)(plant_nn_data_t *);
  int (*process)(plant_nn_data_t *,short *,short *);
  int (*get_samp_rate)(plant_nn_data_t *);
  void (*destroy)(plant_nn_data_t *);
};
extern const struct plant_nn_iface *plant_nn_esp_nsnet_handle_from_name(char *name);
int plant_nn_port_open(void);
void plant_nn_port_close(void);
#ifdef __XTENSA__
_Static_assert(sizeof(struct plant_nn_iface)==20,"SDK2.3.1 iface size");
_Static_assert(offsetof(struct plant_nn_iface,process)==8,"SDK process offset");
_Static_assert(offsetof(struct plant_nn_iface,destroy)==16,"SDK destroy offset");
#endif
