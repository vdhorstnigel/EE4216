#include "time_sync.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

bool time_is_synced(void)
{
    time_t now = 0;
    struct tm tm_info = {0};
    time(&now);
    localtime_r(&now, &tm_info);
    // Consider time valid if year >= 2024 (adjust to your needs)
    return (tm_info.tm_year + 1900) >= 2024;
}

bool time_sync_block_until_synced(uint32_t timeout_ms)
{
    // Prefer the esp-netif helper if available
    esp_err_t r = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (r == ESP_OK) return true;

    // Fallback: poll the wall clock
    const TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < wait_ticks) {
        if (time_is_synced()) return true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return time_is_synced();
}