#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Global flag to hint other subsystems (e.g., LCD) that a high-traffic network
// operation is in progress; they can temporarily throttle or pause.
extern volatile bool g_sending_telegram;

#ifdef __cplusplus
}
#endif
#pragma once
#include <stdbool.h>

// Global runtime flags for telemetry operations (e.g., Telegram send)
// Used by display and other high-bandwidth tasks to throttle during network I/O
extern volatile bool g_sending_telegram;
