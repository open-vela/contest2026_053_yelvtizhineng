/* Checked atomic scalar preferences, separate from credentials. */
#ifndef PLANT_USER_PREFERENCES_H
#define PLANT_USER_PREFERENCES_H
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#ifndef PLANT_PREF_ROOT
#define PLANT_PREF_ROOT "/data/box_voice"
#endif
#define PLANT_PREF_VOLUME_PATH PLANT_PREF_ROOT "/plant_volume.cfg"
#define PLANT_PREF_AE_PATH PLANT_PREF_ROOT "/plant_camera_ae.cfg"
#define PLANT_PREF_FLIP_PATH PLANT_PREF_ROOT "/plant_camera_flip.cfg"
static inline int plant_pref_load_int(const char *path,int fallback,int minimum,int maximum)
{
  char line[64],*end;
  long value;
  int extra,bad;
  FILE *fp=fopen(path,"r");
  /* Existing SD preferences are compatibility inputs; never modify them. */
  if(fp==NULL && !strncmp(path,"/data/",6)){
    char legacy[160];snprintf(legacy,sizeof(legacy),"/mnt/sd/%s",strrchr(path,'/')+1);
    fp=fopen(legacy,"r");
  }
  if(fp==NULL)return fallback;
  if(fgets(line,sizeof(line),fp)==NULL){fclose(fp);return fallback;}
  extra=fgetc(fp);
  bad=ferror(fp);
  if(fclose(fp)!=0 || bad || extra!=EOF)return fallback;
  errno=0;
  value=strtol(line,&end,10);
  if(errno || end==line)return fallback;
  while(*end==' ' || *end=='\t' || *end=='\r' || *end=='\n')end++;
  if(*end || value<minimum || value>maximum)return fallback;
  return (int)value;
}
static inline int plant_pref_save_int(const char *path,int value)
{
  char tmp[160];
  FILE *fp;
  int rc=0;
  if(mkdir(PLANT_PREF_ROOT,0700)<0 && errno!=EEXIST)return -errno;
  if(snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp))return -ENAMETOOLONG;
  fp=fopen(tmp,"w");
  if(fp==NULL)return -errno;
  if(fprintf(fp,"%d\n",value)<0 || fflush(fp)!=0)rc=-EIO;
  if(fclose(fp)!=0)rc=-EIO;
  if(rc==0 && rename(tmp,path)!=0)rc=-errno;
  if(rc<0)remove(tmp);
  return rc;
}
#endif
