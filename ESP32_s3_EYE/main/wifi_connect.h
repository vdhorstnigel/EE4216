// Wi-Fi connection interface
// Ensure NVS, esp_netif, and default event loop are initialized before calling.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init(void);
void static_wifi_init(void);
void nus_wifi_init(void);

#ifdef __cplusplus
}
#endif
