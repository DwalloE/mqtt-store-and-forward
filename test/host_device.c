/*
 * host_device.c - the "device" the chaos suite abuses: the exact
 * saf_core.c the ESP32 runs, bound to the file store (its only memory
 * across "reboots") and to mqtt_mini over a real TCP socket that the
 * chaos proxy cuts whenever it feels like it.
 *
 * Everything chaos.py knows, it learns from this process's stdout:
 *   saf-boot:      store recovered, depth and next sequence
 *   saf-recovered: torn records dropped at boot (the power-cut proof)
 *   saf-enq: N     sequence N durably stored - the ground truth the
 *                  subscriber's verdict is measured against
 *   saf-session:   CONNACK's session-present flag
 *   saf-done:      everything enqueued was acked; exits 0
 *   saf-timeout:   --run-ms expired first; exits 3
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../main/saf_core.h"
#include "file_store.h"
#include "mqtt_mini.h"

typedef struct {
    file_store fs;
    mqtt_mini  mm;
    const char *host;
    int         port;
    char        topic[64];
    bool want_connect;  /* engine asked; the loop does the blocking part */
    bool connected;
    uint16_t last_pid;
    uint32_t last_seq;
} binding;

/* --------------------------------------------------- transport binding -- */
static bool b_connect(void *ctx)
{
    binding *b = ctx;
    b->want_connect = true; /* engine sees an async transport */
    return true;
}

static void b_disconnect(void *ctx)
{
    binding *b = ctx;
    mm_close(&b->mm);
    b->connected = false;
}

static bool b_publish(void *ctx, const char *id, uint32_t seq,
                      const void *payload, size_t len, bool retry)
{
    binding *b = ctx;
    /* The wire envelope adds the idempotent identity; the stored record
     * stays pure application bytes. */
    char wire[256];
    int n = snprintf(wire, sizeof wire,
                     "{\"id\":\"%s\",\"seq\":%u,\"data\":\"%.*s\"}",
                     id, (unsigned)seq, (int)len, (const char *)payload);
    b->last_pid = (uint16_t)(seq % 65535u) + 1u;
    b->last_seq = seq;
    return mm_publish_qos1(&b->mm, b->topic, wire, (size_t)n,
                           b->last_pid, retry);
}

/* --------------------------------------------------------- clock + rand -- */
static uint32_t b_now_ms(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t b_rand(void *ctx)
{
    (void)ctx;
    return (uint32_t)random();
}

/* ------------------------------------------------------------------ main -- */
static const char *arg(int argc, char **argv, const char *name,
                       const char *dflt)
{
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return dflt;
}

static bool flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return true;
    return false;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0); /* chaos reads us line by line */
    srandom((unsigned)(getpid() ^ time(NULL)));

    binding b = {0};
    b.host = arg(argc, argv, "--host", "127.0.0.1");
    b.port = atoi(arg(argc, argv, "--port", "1883"));
    const char *store  = arg(argc, argv, "--store", "store");
    const char *device = arg(argc, argv, "--device", "dev01");
    uint32_t count       = (uint32_t)atoi(arg(argc, argv, "--count", "10"));
    uint32_t interval_ms = (uint32_t)atoi(arg(argc, argv, "--interval-ms", "100"));
    uint32_t run_ms      = (uint32_t)atoi(arg(argc, argv, "--run-ms", "60000"));
    snprintf(b.topic, sizeof b.topic, "saf/%s/telemetry", device);

    saf_ops ops = {0};
    file_store_init(&b.fs, store, &ops);
    ops.tr_connect = b_connect;
    ops.tr_disconnect = b_disconnect;
    ops.tr_publish = b_publish;
    ops.now_ms = b_now_ms;
    ops.rand32 = b_rand;

    saf_config cfg = {
        .device_id = device,
        .max_depth = 64,
        .ack_timeout_ms = (uint32_t)atoi(arg(argc, argv, "--ack-timeout", "2000")),
        .connect_timeout_ms = 2000,
        /* --no-crc-check is the chaos CONTROL: prove the power-cut test
         * DEFEATS a device without the detector before believing the
         * device with it. */
        .validate_crc = !flag(argc, argv, "--no-crc-check"),
    };

    saf s;
    if (!saf_init(&s, &ops, &b, &cfg)) {
        printf("saf-error: init failed\n");
        return 2;
    }
    printf("saf-boot: device=%s depth=%u seq_next=%u crc=%s\n",
           device, s.depth, s.seq_next, cfg.validate_crc ? "on" : "OFF");
    if (s.st.recovered_torn > 0)
        printf("saf-recovered: dropped %u torn record\n",
               s.st.recovered_torn);

    uint32_t started = b_now_ms(NULL);
    uint32_t next_enq = started;
    uint32_t next_stat = started + 2000;
    uint32_t enq_ok = 0;

    for (;;) {
        uint32_t now = b_now_ms(NULL);
        saf_tick(&s);

        if (b.want_connect) {
            b.want_connect = false;
            /* clean_session=false: the broker keeps our QoS1 session
             * state across every outage the chaos suite inflicts. */
            if (mm_connect(&b.mm, b.host, b.port, device,
                           false, 60, 1000)) {
                b.connected = true;
                printf("saf-session: present=%d\n",
                       b.mm.session_present ? 1 : 0);
                saf_on_conn_up(&s);
            } else {
                saf_on_conn_down(&s);
            }
        }

        if (b.connected) {
            uint16_t pid;
            int r = mm_poll(&b.mm, 2, &pid);
            if (r < 0) {
                mm_close(&b.mm);
                b.connected = false;
                saf_on_conn_down(&s);
            } else if (r == 1 && pid == b.last_pid) {
                saf_on_puback(&s, b.last_seq);
            }
        } else {
            usleep(2000);
        }

        if (enq_ok < count && (int32_t)(now - next_enq) >= 0) {
            char data[64];
            snprintf(data, sizeof data, "t=%u n=%u",
                     (unsigned)(now - started), (unsigned)enq_ok);
            uint32_t seq = s.seq_next; /* the number enqueue will assign */
            if (!saf_enqueue(&s, data, strlen(data))) {
                printf("saf-error: enqueue refused at n=%u\n",
                       (unsigned)enq_ok);
                return 4;
            }
            printf("saf-enq: %u\n", (unsigned)seq);
            enq_ok++;
            next_enq += interval_ms;
        }

        if ((int32_t)(now - next_stat) >= 0) {
            next_stat += 2000;
            printf("saf-stat: depth=%u sent=%u acked=%u link=%d\n",
                   s.depth, s.st.sent, s.st.acked, (int)s.link);
        }

        if (enq_ok >= count && s.depth == 0 && !s.inflight) {
            printf("saf-done: all enqueued and drained (acked=%u)\n",
                   s.st.acked);
            return 0;
        }
        if (now - started >= run_ms) {
            printf("saf-timeout: undrained depth=%u after %u ms\n",
                   s.depth, (unsigned)run_ms);
            return 3;
        }
    }
}
