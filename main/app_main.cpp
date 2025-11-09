#include "frame_cap_pipeline.hpp"
#include "who_recognition_app_lcd.hpp"
#include "who_recognition_app_term.hpp"
#include "who_spiflash_fatfs.hpp"
#include "wifi_connect.h"
#include "http_streamer.h"
#include "MyRecognitionApp.hpp"
#include "recognition_control.h"
#include "net_sender.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

using namespace who::frame_cap;
using namespace who::app;

SemaphoreHandle_t task_semaphore;
static const char *TAG = "app_main";

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

extern "C" void init(void *pvParameter) {

    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(2000)); // wait for wifi to settle
    ntp_sync();

    // Start asynchronous network sender (pinned to core 1)
    (void)net_sender_start(1);

    start_webserver();
    start_motion();

    xSemaphoreGive(task_semaphore);
    vTaskDelete(NULL);
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
    // init all basic components for wifi
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    xTaskCreate(init, "init", 4096, NULL, 5, NULL);

    xSemaphoreTake(task_semaphore, portMAX_DELAY);

    auto frame_cap = get_dvp_frame_cap_pipeline();

    auto recognition_app = new MyRecognitionApp(frame_cap);
    recognition_app->run();
}