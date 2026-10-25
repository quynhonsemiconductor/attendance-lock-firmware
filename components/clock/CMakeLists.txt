idf_component_register(SRCS "clock.c" "clock_port_esp.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_timer lwip esp_system bootloader_support)
