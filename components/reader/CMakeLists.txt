idf_component_register(SRCS "reader_logic.c" "reader_esp.c" "rc522.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_timer esp_hw_support log)
