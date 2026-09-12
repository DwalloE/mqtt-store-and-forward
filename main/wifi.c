/*
 * wifi.c - see wifi.h. The usual STA dance: netif + event loop, wait
 * on an event group for GOT_IP, retry on disconnect (the MQTT layer's
 * backoff governs the broker; Wi-Fi itself just keeps trying).
 */
#include "wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "saf_wifi";
static EventGroupHandle_t s_group;
#define GOT_IP_BIT BIT0

static void on_event(void *arg, esp_event_base_t base,
                     int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_group, GOT_IP_BIT);
    }
}

void wifi_connect_blocking(void)
{
    s_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_event, NULL));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, CONFIG_SAF_WIFI_SSID,
            sizeof cfg.sta.ssid - 1);
    strncpy((char *)cfg.sta.password, CONFIG_SAF_WIFI_PASSWORD,
            sizeof cfg.sta.password - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "joining %s", CONFIG_SAF_WIFI_SSID);
    xEventGroupWaitBits(s_group, GOT_IP_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    ESP_LOGI(TAG, "got ip");
}
