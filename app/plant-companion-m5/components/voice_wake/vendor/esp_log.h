#pragma once
#include <stdio.h>
#define ESP_LOGE(tag,fmt,...) printf("[Wake-model] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGI(tag,fmt,...) ((void)0)
#define ESP_LOGW(tag,fmt,...) ((void)0)
#define ESP_LOGD(tag,fmt,...) ((void)0)
