idf_component_register(SRCS "power.c" "power_esp.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_adc esp_hw_support esp_system log)
