idf_component_register(SRCS "main.c"
                       REQUIRES reader lock store sync clock provision power door
                       PRIV_REQUIRES esp_timer nvs_flash driver esp_hw_support)
