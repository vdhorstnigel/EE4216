/* time_sync.h - SNTP time sync helpers */
#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>
#ifndef __cplusplus
/* Fallback in case <stdbool.h> didn't define bool (older compiler edge case) */
#ifndef bool
typedef _Bool bool;
#endif
#endif
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize SNTP client using esp_netif. Non-blocking; safe to call multiple times. */
void app_sntp_init(void);

/* Returns true if system time appears synced (year >= 2024 or explicit sync flag set). */
bool time_is_synced(void);

/* Block until time is synced or timeout_ms elapses. Returns true if synced. */
bool time_sync_block_until_synced(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* TIME_SYNC_H */