idf_component_register(
    SRCS "main.c" "protocol.c" "command.c" "wifi_sta.c" "camera.c" "web_server.c" "mdns_adv.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_http_server esp_netif esp_event nvs_flash json esp32-camera esp_timer espressif__mdns
)
