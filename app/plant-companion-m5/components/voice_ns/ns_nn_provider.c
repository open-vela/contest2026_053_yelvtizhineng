/* Same SDK2.3.1 static model provider layout, with ESP_PLATFORM partition field.
 * Model file bytes are copied unchanged to16-byte aligned general PSRAM.
 * Provider is private to the namespaced NN engine and never replaces wake data.
 */
#include "ns_nn_iface.h"
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
struct nn_model_data {int num;char **files;char **data;int *sizes;};
struct nn_model_list
{
  char **model_name;char **model_info;void *partition;void *mmap_handle;
  int num;struct nn_model_data **model_data;
};
struct nn_provider
{
  struct nn_model_list list;struct nn_model_data files;
  struct nn_model_data *models[1];char *names[1],*infos[1];
  char *file_names[3],*file_data[3];int file_sizes[3];
  char model_name[8],model_info[40];
};
extern const unsigned char plant_nn_asset_data[],plant_nn_asset_index[],plant_nn_asset_info[];
static struct nn_provider *g_nn_provider;
extern int plant_nn_alloc_begin(void);
extern int plant_nn_alloc_report(void);
#ifdef __XTENSA__
_Static_assert(sizeof(struct nn_model_list)==24,"ESP_PLATFORM SRMODEL list ABI");
_Static_assert(offsetof(struct nn_model_list,num)==16,"SRMODEL num offset");
_Static_assert(offsetof(struct nn_model_list,model_data)==20,"SRMODEL files offset");
_Static_assert(sizeof(struct nn_model_data)==16,"SRMODEL file table ABI");
#endif
struct nn_model_list *plant_nn_port_get_static_srmodels(void)
{return g_nn_provider?&g_nn_provider->list:NULL;}
char *plant_nn_port_get_model_base_path(void){return NULL;}
int plant_nn_port_open(void)
{
  if(g_nn_provider)return -EBUSY;
  if(plant_nn_alloc_begin()<0)return -EBUSY;
  struct nn_provider *p=calloc(1,sizeof(*p));if(!p)return -ENOMEM;
  const unsigned char *assets[]={plant_nn_asset_data,plant_nn_asset_index,plant_nn_asset_info};
  const unsigned sizes[]={337349,360,81};
  const char *names[]={"nsnet2_data","nsnet2_index","_MODEL_INFO_"};
  for(unsigned i=0;i<3;i++)
    {
      size_t reserved=(sizes[i]+63u)&~15u;
      p->file_data[i]=memalign(16,reserved);
      if(!p->file_data[i])
        {for(unsigned j=0;j<i;j++)free(p->file_data[j]);free(p);return -ENOMEM;}
      memset(p->file_data[i],0,reserved);memcpy(p->file_data[i],assets[i],sizes[i]);
      p->file_sizes[i]=(int)sizes[i];p->file_names[i]=(char *)names[i];
    }
  strcpy(p->model_name,"nsnet2");strcpy(p->model_info,"nsnet2_v1_ns_0_0.0_0.0");
  p->names[0]=p->model_name;p->infos[0]=p->model_info;p->models[0]=&p->files;
  p->files.num=3;p->files.files=p->file_names;p->files.data=p->file_data;p->files.sizes=p->file_sizes;
  p->list.model_name=p->names;p->list.model_info=p->infos;p->list.num=1;p->list.model_data=p->models;
  g_nn_provider=p;return 0;
}
void plant_nn_port_close(void)
{
  struct nn_provider *p=g_nn_provider;g_nn_provider=NULL;
  if(p){for(unsigned i=0;i<3;i++)free(p->file_data[i]);free(p);plant_nn_alloc_report();}
}
