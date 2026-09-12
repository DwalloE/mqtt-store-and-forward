/*
 * fake_transport.c - see fake_transport.h.
 */
#include "fake_transport.h"

#include "esp_timer.h"

#define CONNACK_DELAY_MS 150
#define PUBACK_DELAY_MS  60

static struct {
    bool online;        /* the lie the shell controls */
    bool connack_due;
    bool puback_due;
    int64_t connack_at_us;
    int64_t puback_at_us;
    uint32_t puback_seq;
} f = { .online = true };

static bool f_connect(void *ctx)
{
    (void)ctx;
    if (!f.online)
        return false; /* radio "dead": refused immediately */
    f.connack_due = true;
    f.connack_at_us = esp_timer_get_time() + CONNACK_DELAY_MS * 1000;
    return true;
}

static void f_disconnect(void *ctx)
{
    (void)ctx;
    f.connack_due = false;
    f.puback_due = false;
}

static bool f_publish(void *ctx, const char *id, uint32_t seq,
                      const void *payload, size_t len, bool retry)
{
    (void)ctx; (void)id; (void)payload; (void)len; (void)retry;
    if (!f.online)
        return false;
    f.puback_due = true;
    f.puback_at_us = esp_timer_get_time() + PUBACK_DELAY_MS * 1000;
    f.puback_seq = seq;
    return true;
}

void fake_transport_init(saf_ops *ops)
{
    ops->tr_connect = f_connect;
    ops->tr_disconnect = f_disconnect;
    ops->tr_publish = f_publish;
}

void fake_transport_poll(saf *s)
{
    int64_t now = esp_timer_get_time();
    if (f.connack_due && now >= f.connack_at_us) {
        f.connack_due = false;
        saf_on_conn_up(s);
    }
    if (f.puback_due && now >= f.puback_at_us) {
        f.puback_due = false;
        saf_on_puback(s, f.puback_seq);
    }
}

void fake_transport_set_online(saf *s, bool online)
{
    f.online = online;
    if (!online) {
        f.connack_due = false;
        f.puback_due = false;
        saf_on_conn_down(s); /* an established session drops on the spot */
    }
}

bool fake_transport_online(void)
{
    return f.online;
}
