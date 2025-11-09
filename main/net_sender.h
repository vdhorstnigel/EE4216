#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize and start the background network sender task.
// If already started, this is a no-op and returns true.
// core_id: 0 or 1 on dual-core targets. Recommended: 1.
bool net_sender_start(int core_id);

// Enqueue an HTTP plain-text POST to http://ip:port/path with the given body.
// Data is copied internally, so the provided pointers can be transient.
bool net_send_http_plain_async(const char *ip,
                               uint16_t port,
                               const char *path,
                               const char *body,
                               size_t len);

// Enqueue a Telegram send of an RGB565 image. The function takes ownership of
// the rgb565 buffer and will free() it after sending (or on failure).
// The caption string is copied internally.
bool net_send_telegram_rgb565_take(uint8_t *rgb565,
                                   size_t rgb565_len,
                                   uint16_t width,
                                   uint16_t height,
                                   uint8_t quality,
                                   const char *caption);

#ifdef __cplusplus
}
#endif
