idf_component_register(SRCS "sleep_plan.c" "sleep_esp.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_hw_support esp_system log)
