idf_component_register(SRCS "main.c"
                       REQUIRES reader lock store sync clock provision power
                       PRIV_REQUIRES esp_timer nvs_flash esp_hw_support esp_driver_gpio)
