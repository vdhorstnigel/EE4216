#include "frame_cap_pipeline.hpp"
#include "who_recognition_app_lcd.hpp"
#include "who_recognition_app_term.hpp"
#include "who_spiflash_fatfs.hpp"
#include "wifi_connect.c"
#include "http_streamer.h"
#include "MyRecognitionApp.hpp"
#include "recognition_control.h"
#include "time_sync.h"

using namespace who::frame_cap;
using namespace who::app;

extern "C" void app_main(void)
{
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

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    nus_wifi_init();
    app_sntp_init();

    vTaskDelay(pdMS_TO_TICKS(5000)); // wait for wifi connection
    start_webserver();
#if CONFIG_IDF_TARGET_ESP32S3
    auto frame_cap = get_dvp_frame_cap_pipeline();
#elif CONFIG_IDF_TARGET_ESP32P4
    auto frame_cap = get_mipi_csi_frame_cap_pipeline();
    // auto frame_cap = get_uvc_frame_cap_pipeline();
#endif
    auto recognition_app = new MyRecognitionApp(frame_cap);
    recognition_app->run();
    // Register recognition event group for HTTP action control buttons
    recognition_register_event_group(recognition_app->get_recognition_event_group());
}
