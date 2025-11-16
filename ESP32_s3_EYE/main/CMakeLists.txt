set(src_dirs        ./)

set(include_dirs    ./)

set(requires who_spiflash_fatfs
             who_recognition_app
             esp_http_server
             esp32-camera
             esp_wifi
             nvs_flash
             wpa_supplicant
             esp-dl
             esp_http_client
             lwip
             )

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})




