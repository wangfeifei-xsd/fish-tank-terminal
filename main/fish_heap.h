#pragma once

#include <stdbool.h>

/** Minimum largest free blocks before starting HTTPS / SDIO-heavy work. */
#define FISH_HEAP_MIN_DMA_LARGEST  12288u
#define FISH_HEAP_MIN_INT_LARGEST  28672u

bool fish_heap_sdio_safe(void);
void fish_heap_log_unsafe(const char *tag);
