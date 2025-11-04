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
