idf_component_register(
    SRCS "main.c"
         "saf_core.c"
         "nvs_store.c"
         "fake_transport.c"
         "mqtt_transport.c"
         "wifi.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver nvs_flash mqtt esp_wifi esp_netif esp_event
                  esp_timer esp_hw_support)
