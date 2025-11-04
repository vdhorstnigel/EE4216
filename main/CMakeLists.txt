set(include_dirs    ./)

# Explicitly list all source files for this component to ensure they are compiled and linked
set(srcs
    app_main.cpp
    frame_cap_pipeline.cpp
    http_streamer.c
    wifi_connect.c
    recognition_control.cpp
    telegram_sender.c
    credentials.c
    time_sync.c
)

set(requires
    who_spiflash_fatfs
    who_recognition_app
    who_frame_lcd_disp
    esp_http_server
    esp_http_client
    esp-tls
    esp32-camera
    esp_netif
    esp_wifi
    nvs_flash
    wpa_supplicant
    esp-dl
)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})




