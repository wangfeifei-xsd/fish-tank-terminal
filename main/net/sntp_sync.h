#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool fish_sntp_sync(int timeout_ms);
/** True only after real NTP (not TLS fallback clock). Required for HMAC APIs. */
bool fish_sntp_is_authoritative(void);
bool fish_sntp_wait_authoritative(int timeout_ms);
int64_t fish_time_ms(void);
bool fish_time_ready(void);

#ifdef __cplusplus
}
#endif
