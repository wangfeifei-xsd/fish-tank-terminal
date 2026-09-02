#include "fish_heap.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

bool fish_heap_sdio_safe(void)
{
    size_t dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    return dma >= FISH_HEAP_MIN_DMA_LARGEST && internal >= FISH_HEAP_MIN_INT_LARGEST;
}

void fish_heap_log_unsafe(const char *tag)
{
    ESP_LOGW(tag, "skip HTTPS/SDIO: low heap (dma=%u int=%u need dma>=%u int>=%u)",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)FISH_HEAP_MIN_DMA_LARGEST,
             (unsigned)FISH_HEAP_MIN_INT_LARGEST);
}
