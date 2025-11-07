#include "frame_cap_pipeline.hpp"
#include "who_recognition_app_lcd.hpp"
#include "who_recognition_app_term.hpp"
#include "who_spiflash_fatfs.hpp"
#include "wifi_connect.c"
#include "http_streamer.h"
#include "MyRecognitionApp.hpp"
#include "recognition_control.h"
#include "net_sender.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

using namespace who::frame_cap;
using namespace who::app;

extern "C" void ntp_sync() {
    time_t now = 0;
    struct tm timeinfo = { 0 };

    sntp_set_sync_interval(1 * 60 * 60 * 1000UL);  // 1 hour
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "0.sg.pool.ntp.org");
    esp_sntp_setservername(1, "1.sg.pool.ntp.org");
    esp_sntp_setservername(2, "2.sg.pool.ntp.org");
    esp_sntp_init();

    setenv("TZ", "CST-8", 1);
    tzset();

    for (int retry = 0; retry < 20; retry++) {
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year >= (2025 - 1900)) {
            ESP_LOGI("SNTP", "Time is synchronized: %lld", now);
            return;
        }

        ESP_LOGI("SNTP", "Waiting for time to be synchronized... (%d/10)", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGE("SNTP", "Failed to synchronize time after 40s.");
}

extern "C" bool try_wifi_connect(int timeout_ms) {
    wifi_init();
    int waited = 0;
    const int delay_step = 500; // ms

    while (!WIFI_CONNECTED && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(delay_step));
        waited += delay_step;
    }

    return WIFI_CONNECTED;
}

extern "C" void init(void *pvParameter) {

    bool wifi_ok = try_wifi_connect(10000);
    ESP_LOGI(TAG, "wifi_connected = %d", wifi_ok);
    if (wifi_ok) {
        init_sntp();
        wait_for_ntp_sync();
        ESP_LOGI("TIME", "Epoch time: %lld", get_epoch_time());
    } else {
        ESP_LOGW(TAG, "WiFi not connected, going to offline mode.");
    }

    // Start asynchronous network sender (pinned to core 1)
    (void)net_sender_start(1);

    start_webserver();

    xSemaphoreGive(task_semaphore);
    vTaskDelete(NULL);
}


extern "C" void main_loop(void *pvParameter) {
    ESP_LOGI("main loop", "Waiting for init to complete...");
    xSemaphoreTake(task_semaphore, portMAX_DELAY);

    auto recognition_app = new MyRecognitionApp(frame_cap);
    recognition_app->run();
    // Register recognition event group for HTTP action control buttons
    recognition_register_event_group(recognition_app->get_recognition_event_group());
    
}

extern "C" void app_main(void)
{
    task_semaphore = xSemaphoreCreateBinary();
    
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);
#if CONFIG_DB_FATFS_FLASH
    ESP_ERROR_CHECK(fatfs_flash_mount());
#elif CONFIG_DB_SPIFFS
    ESP_ERROR_CHECK(bsp_spiffs_mount());
#endif
#if CONFIG_DB_FATFS_SDCARD || CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD || CONFIG_HUMAN_FACE_FEAT_MODEL_IN_SDCARD
    ESP_ERROR_CHECK(bsp_sdcard_mount());
#endif

// close led
#ifdef BSP_BOARD_ESP32_S3_EYE
    ESP_ERROR_CHECK(bsp_leds_init());
    ESP_ERROR_CHECK(bsp_led_set(BSP_LED_GREEN, false));
#endif

    static const char *TAG = "app_main";

    // init all basic components for wifi
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
#if CONFIG_IDF_TARGET_ESP32S3
    auto frame_cap = get_dvp_frame_cap_pipeline();
#elif CONFIG_IDF_TARGET_ESP32P4
    auto frame_cap = get_mipi_csi_frame_cap_pipeline();
    // auto frame_cap = get_uvc_frame_cap_pipeline();
#endif

    xTaskCreate(init, "init", 4096, NULL, 5, NULL);
    xTaskCreate(main_loop, "main_loop", 4096, NULL, 5, NULL);
}
