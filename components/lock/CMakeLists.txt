idf_component_register(SRCS "lock.c" "lock_esp.c" "lock_math.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES driver esp_timer log)
