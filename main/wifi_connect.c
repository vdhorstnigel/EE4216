#include <stdio.h> //for basic printf commands
#include <string.h> //for handling strings
#include "freertos/FreeRTOS.h" //for delay,mutexs,semphrs rtos operations
#include "esp_system.h" //esp_init funtions esp_err_t 
#include "esp_wifi.h" //esp_wifi_init functions and wifi operations
#include "esp_log.h" //for showing logs
#include "esp_event.h" //for wifi event
#include "nvs_flash.h" //non volatile storage
#include "lwip/err.h" //light weight ip packets error handling
#include "lwip/sys.h" //system applications for light weight ip apps
#include <stdbool.h>
#include "esp_eap_client.h"
#include "credentials.c"

static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,void *event_data){
const char *TAG = "wifi_init";
if(event_id == WIFI_EVENT_STA_START)
{
  printf("WIFI CONNECTING....\n");
}
else if (event_id == WIFI_EVENT_STA_CONNECTED)
{
  printf("WiFi CONNECTED\n");
}
else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
{
  wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*) event_data;
  printf("WiFi lost connection. Reason: %d\n", disconn->reason);
}
else if (event_id == IP_EVENT_STA_GOT_IP)
{
  ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
  ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
}
}


void wifi_init()
{
    esp_netif_create_default_wifi_sta();  
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_initiation)); //     
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    wifi_config_t wifi_configuration = {
        .sta = {
            .ssid = "",
            .password = "",
            .pmf_cfg = {
            .capable = true,
            .required = false
           }
           }
        };
    strcpy((char*)wifi_configuration.sta.ssid, ssid);
    strcpy((char*)wifi_configuration.sta.password, password);  
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration));
    // 3 - Wi-Fi Start Phase
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    //wifi_scan();
    // 4- Wi-Fi Connect Phase
    ESP_ERROR_CHECK(esp_wifi_connect());
    printf( "wifi_init_sta finished. SSID:%s\n",ssid);
}

void nus_wifi_init()
{
    esp_netif_create_default_wifi_sta();  
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_initiation)); //     
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    wifi_config_t wifi_configuration = {
        .sta = {
            .ssid = "",
            .pmf_cfg = {
            .capable = true,
            .required = false
           }
           }
        };
    strcpy((char*)wifi_configuration.sta.ssid, NUS_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_eap_client_set_identity((uint8_t *)NUS_identity, strlen(NUS_identity)));
    ESP_ERROR_CHECK(esp_eap_client_set_username((uint8_t *)NUS_username, strlen(NUS_username)));
    ESP_ERROR_CHECK(esp_eap_client_set_password((uint8_t *)NUS_password, strlen(NUS_password)));
    ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
    ESP_ERROR_CHECK(esp_wifi_connect());
    printf( "wifi_init_sta finished. WPA2 PEAP SSID:%s \n",NUS_ssid);
}

