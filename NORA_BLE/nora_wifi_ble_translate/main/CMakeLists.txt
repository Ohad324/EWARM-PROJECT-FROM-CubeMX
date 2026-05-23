idf_component_register(SRCS "nora_ble_bridge.c"
                             "music_task.c"
                             "cloud_upload.c"
                             "command_router.c"
                             "pc_discovery.c"
                             "ntfy_client.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash driver esp_wifi esp_event esp_netif
                              esp_http_client json mbedtls lwip esp-tls)
