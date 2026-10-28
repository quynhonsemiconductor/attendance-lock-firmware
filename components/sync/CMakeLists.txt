idf_component_register(SRCS "sync.c" "sync_json.c" "sync_port_esp.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES store clock power nvs_flash esp_wifi esp_http_client
                                       esp_netif esp-tls esp_app_format)
