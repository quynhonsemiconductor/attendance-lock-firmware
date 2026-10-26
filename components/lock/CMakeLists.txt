idf_component_register(SRCS "lock.c" "lock_esp.c" "lock_math.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_timer log)
