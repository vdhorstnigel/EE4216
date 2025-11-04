#include "time_sync.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG_TS = "time_sync";

void app_sntp_init(void)
{
    // Initialize SNTP with a common pool server. You can customize this if needed.
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_TS, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG_TS, "esp_netif_sntp_start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG_TS, "SNTP started");
}