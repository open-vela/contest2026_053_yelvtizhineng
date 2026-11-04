#pragma once
#include <stdint.h>
#include <stddef.h>
#define MALLOC_CAP_SPIRAM (1<<10)
void *heap_caps_malloc(size_t size,uint32_t caps);
void *heap_caps_calloc(size_t n,size_t size,uint32_t caps);
void heap_caps_free(void *ptr);
