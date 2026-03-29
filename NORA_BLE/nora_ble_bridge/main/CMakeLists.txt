idf_component_register(SRCS "nora_ble_bridge.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash driver)
