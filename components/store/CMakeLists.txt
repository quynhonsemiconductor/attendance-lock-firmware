idf_component_register(SRCS "store.c" "store_port_esp.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_partition esp_hw_support)
