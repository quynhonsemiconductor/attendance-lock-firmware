idf_component_register(SRCS "provision.c" "provision_port_esp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash)
