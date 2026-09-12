/*
 * mqtt_transport.c - see mqtt_transport.h.
 */
#include "mqtt_transport.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "wifi.h"

static const char *TAG = "saf_mqtt";

typedef enum { EV_UP, EV_DOWN, EV_PUBACK } ev_kind;
typedef struct {
    ev_kind kind;
    int     msg_id;
} ev_t;

static esp_mqtt_client_handle_t s_client;
static QueueHandle_t s_events;
static char s_topic[64];
static bool s_started;

/* single in-flight message: esp-mqtt msg_id of the last publish and
 * the engine sequence it carries */
static int      s_last_msg_id = -1;
static uint32_t s_last_seq;

static void on_mqtt_event(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = event_data;
    ev_t ev;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ev.kind = EV_UP;
        xQueueSend(s_events, &ev, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
    case MQTT_EVENT_ERROR:
        ev.kind = EV_DOWN;
        xQueueSend(s_events, &ev, 0);
        break;
    case MQTT_EVENT_PUBLISHED:
        ev.kind = EV_PUBACK;
        ev.msg_id = e->msg_id;
        xQueueSend(s_events, &ev, 0);
        break;
    default:
        break;
    }
}

static bool r_connect(void *ctx)
{
    (void)ctx;
    if (!s_started) {
        s_started = true;
        return esp_mqtt_client_start(s_client) == ESP_OK;
    }
    return esp_mqtt_client_reconnect(s_client) == ESP_OK;
}

static void r_disconnect(void *ctx)
{
    (void)ctx;
    esp_mqtt_client_disconnect(s_client);
}

static bool r_publish(void *ctx, const char *id, uint32_t seq,
                      const void *payload, size_t len, bool retry)
{
    (void)ctx; (void)retry; /* esp-mqtt owns the wire-level DUP flag */
    char wire[256];
    int n = snprintf(wire, sizeof wire,
                     "{\"id\":\"%s\",\"seq\":%u,\"data\":\"%.*s\"}",
                     id, (unsigned)seq, (int)len, (const char *)payload);
    int msg_id = esp_mqtt_client_publish(s_client, s_topic, wire, n, 1, 0);
    if (msg_id < 0)
        return false;
    s_last_msg_id = msg_id;
    s_last_seq = seq;
    return true;
}

void mqtt_transport_init(saf_ops *ops, const char *device_id)
{
    wifi_connect_blocking();

    snprintf(s_topic, sizeof s_topic, "saf/%s/telemetry", device_id);
    s_events = xQueueCreate(8, sizeof(ev_t));
    configASSERT(s_events);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_SAF_BROKER_URI,
        .credentials.client_id = device_id,
        /* the two decisions docs/protocol-choice.md defends: */
        .session.disable_clean_session = true,  /* resume QoS1 state */
        .network.disable_auto_reconnect = true, /* engine owns backoff */
    };
    s_client = esp_mqtt_client_init(&cfg);
    configASSERT(s_client);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL));
    ESP_LOGI(TAG, "broker %s topic %s", CONFIG_SAF_BROKER_URI, s_topic);

    ops->tr_connect = r_connect;
    ops->tr_disconnect = r_disconnect;
    ops->tr_publish = r_publish;
}

void mqtt_transport_poll(saf *s)
{
    ev_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        switch (ev.kind) {
        case EV_UP:
            saf_on_conn_up(s);
            break;
        case EV_DOWN:
            saf_on_conn_down(s);
            break;
        case EV_PUBACK:
            if (ev.msg_id == s_last_msg_id)
                saf_on_puback(s, s_last_seq);
            break;
        }
    }
}
