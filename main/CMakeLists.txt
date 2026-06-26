set(MODULES_SRCS 
    "./modules/network/wifi_station.cpp"
    "./modules/network/http_requester.cpp"
)

set (ESP_REQUIRES
    nvs_flash
    esp_wifi
    esp_event 
    esp_netif 
    esp_http_client
)

idf_component_register(SRCS "main.cpp" ${MODULES_SRCS}
                    PRIV_REQUIRES ${ESP_REQUIRES}
                    INCLUDE_DIRS ".")