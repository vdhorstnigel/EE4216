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
#include "esp_netif.h" // network interface (DHCP/static IP)
#include "lwip/ip4_addr.h" // IPv4 helpers (IP4_ADDR, ip4_addr_t)
#include "lwip/def.h" // PP_HTONL, LWIP_MAKEU32
#include <stdbool.h>
#include "esp_eap_client.h"
#include "credentials.h"
#include "wifi_connect.h"

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


void static_wifi_init()
{
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();  
  wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_initiation)); //     
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

  wifi_config_t wifi_configuration; 
  memset(&wifi_configuration, 0, sizeof(wifi_configuration));
  wifi_configuration.sta.pmf_cfg.capable = true;
  wifi_configuration.sta.pmf_cfg.required = false;

  // Configure static IPv4 for the STA interface
  esp_netif_ip_info_t ip_info;
  ip_info.ip.addr = PP_HTONL(LWIP_MAKEU32(10,117,110,15));
  ip_info.gw.addr = PP_HTONL(LWIP_MAKEU32(10,117,110,197));
  ip_info.netmask.addr = PP_HTONL(LWIP_MAKEU32(255,255,255,0));

  // Stop DHCP client and apply the static IP settings before connecting
  esp_err_t dhcp_stop_err = esp_netif_dhcpc_stop(sta_netif);
  if (dhcp_stop_err != ESP_OK && dhcp_stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
      ESP_LOGW("wifi_init", "DHCP stop failed: %s", esp_err_to_name(dhcp_stop_err));
  }
  ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip_info));

  esp_netif_dns_info_t dns0; memset(&dns0, 0, sizeof(dns0));
  dns0.ip.u_addr.ip4.addr = ip_info.gw.addr; // gateway
  ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns0));

  strncpy((char*)wifi_configuration.sta.ssid, ssid, sizeof(wifi_configuration.sta.ssid)-1);
  strncpy((char*)wifi_configuration.sta.password, password, sizeof(wifi_configuration.sta.password)-1);  
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration));
    // 3 - Wi-Fi Start Phase
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    //wifi_scan();
    // 4- Wi-Fi Connect Phase
    ESP_ERROR_CHECK(esp_wifi_connect());
    printf( "wifi_init_sta finished. SSID:%s\n",ssid);
}

void wifi_init()
{
  // Create default STA interface (must be done after esp_netif_init())
  esp_netif_create_default_wifi_sta();  

  wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_initiation));

  // Disable power-save to reduce latency (optional; move after init)
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

  wifi_config_t wifi_configuration; 
  memset(&wifi_configuration, 0, sizeof(wifi_configuration));
  wifi_configuration.sta.pmf_cfg.capable = true;
  wifi_configuration.sta.pmf_cfg.required = false;

  // Safe copy with bounds; guard against NULL pointers
  if (!ssid || !password) {
    ESP_LOGE("wifi_init", "SSID or password is NULL. Check credentials.c definitions and header externs.");
    return; // avoid crashing
  }
  strncpy((char*)wifi_configuration.sta.ssid, ssid, sizeof(wifi_configuration.sta.ssid) - 1);
  strncpy((char*)wifi_configuration.sta.password, password, sizeof(wifi_configuration.sta.password) - 1);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_connect());
  printf("wifi_init_sta finished. SSID:%s\n", ssid);
}

void nus_wifi_init()
{
  esp_netif_create_default_wifi_sta();  
  wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_initiation)); //     
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
  // Zero-initialize to avoid missing-field warnings
  wifi_config_t wifi_configuration; 
  memset(&wifi_configuration, 0, sizeof(wifi_configuration));
  wifi_configuration.sta.pmf_cfg.capable = true;
  wifi_configuration.sta.pmf_cfg.required = false;
  strncpy((char*)wifi_configuration.sta.ssid, NUS_ssid, sizeof(wifi_configuration.sta.ssid)-1);

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

