/*
 * main.c - the ESP32 binding of the store-and-forward engine, wearing
 * 04's supervisor/shell pattern: one control loop that polls the UART
 * shell, ticks the engine, pumps the transport, samples telemetry,
 * feeds the watchdog and heartbeats.
 *
 * Shell: stat | out | in
 *   stat - engine counters (queued/sent/acked/dropped/torn/seq)
 *   out  - the injected transport starts lying: radio "dead"
 *   in   - radio "back"; when everything queued during the outage has
 *          drained, the FIRMWARE prints its own verdict from its own
 *          counters:  saf: drained, nothing lost
 *          (failure text shares no substring: saf DATA AT RISK ...)
 *
 * CI types that exact sequence and waits for the verdict plus the
 * t=10s heartbeat (outliving the 5 s task watchdog, 01's lesson).
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "saf_core.h"
#include "nvs_store.h"
#include "fake_transport.h"
#include "mqtt_transport.h"

static const char *TAG = "saf_main";

#define DEVICE_ID       "esp01"
#define LAP_MS          20
#define SAMPLE_MS       500
#define DRAIN_GRACE_MS  20000
#define SHELL_LINE_MAX  32

static saf s_engine;

/* --------------------------------------------------------------- shell -- */
static const char *link_name(saf_link_state l)
{
    return l == SAF_LINK_UP ? "UP"
         : l == SAF_LINK_CONNECTING ? "CONNECTING" : "DOWN";
}

static void stat_report(void)
{
    const saf *s = &s_engine;
    printf("stat: link=%s depth=%u inflight=%d online=%d\n",
           link_name(s->link), (unsigned)s->depth,
           s->inflight ? 1 : 0, fake_transport_online() ? 1 : 0);
    printf("stat: enq=%u sent=%u acked=%u dropped=%u torn=%u stale=%u\n",
           (unsigned)s->st.enqueued, (unsigned)s->st.sent,
           (unsigned)s->st.acked, (unsigned)s->st.dropped_full,
           (unsigned)s->st.recovered_torn, (unsigned)s->st.stale_acks);
    printf("stat: seq_next=%u ceiling=%u attempt=%u\n",
           (unsigned)s->seq_next, (unsigned)s->seq_ceiling,
           (unsigned)s->attempt);
}

static bool     s_pending_drain;
static uint32_t s_drain_started_queued;
static int64_t  s_drain_deadline_us;

static void cmd_out(void)
{
#if CONFIG_SAF_TRANSPORT_FAKE
    fake_transport_set_online(&s_engine, false);
    printf("transport: forced offline - queueing to NVS\n");
#else
    printf("out/in need the fake transport (menuconfig: SAF_TRANSPORT)\n");
#endif
}

static void cmd_in(void)
{
#if CONFIG_SAF_TRANSPORT_FAKE
    fake_transport_set_online(&s_engine, true);
    s_pending_drain = s_engine.depth > 0;
    s_drain_started_queued = s_engine.depth;
    s_drain_deadline_us = esp_timer_get_time()
                        + (int64_t)DRAIN_GRACE_MS * 1000;
    printf("transport: restored - draining %u queued\n",
           (unsigned)s_engine.depth);
#else
    printf("out/in need the fake transport (menuconfig: SAF_TRANSPORT)\n");
#endif
}

static void shell_banner(void)
{
    printf("shell: commands: stat | out | in\n> ");
    fflush(stdout);
}

static void dispatch(const char *line)
{
    if (strcmp(line, "stat") == 0)     stat_report();
    else if (strcmp(line, "out") == 0) cmd_out();
    else if (strcmp(line, "in") == 0)  cmd_in();
    else if (line[0] != '\0')
        printf("unknown command '%s' - try stat | out | in\n", line);
    printf("> ");
    fflush(stdout);
}

/* Polls UART0 without blocking the lap; echoes, dispatches on EOL
 * (04's shell verbatim). */
static void shell_poll(void)
{
    static char   line[SHELL_LINE_MAX];
    static size_t len;

    uint8_t buf[16];
    int n = uart_read_bytes(UART_NUM_0, buf, sizeof buf, 0);
    for (int i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            putchar('\n');
            line[len] = '\0';
            len = 0;
            dispatch(line);
        } else if (len < sizeof line - 1) {
            putchar(c);
            line[len++] = c;
        }
    }
    if (n > 0) fflush(stdout);
}

/* ----------------------------------------------------- engine plumbing -- */
static uint32_t b_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t b_rand(void *ctx)
{
    (void)ctx;
    return esp_random();
}

/* ------------------------------------------------------------ app_main -- */
void app_main(void)
{
    saf_ops ops = {0};
    nvs_store_init(&ops);
#if CONFIG_SAF_TRANSPORT_FAKE
    fake_transport_init(&ops);
#else
    mqtt_transport_init(&ops, DEVICE_ID);
#endif
    ops.now_ms = b_now_ms;
    ops.rand32 = b_rand;

    const saf_config cfg = {
        .device_id = DEVICE_ID,
        .max_depth = 64,
        .ack_timeout_ms = 5000,
        .connect_timeout_ms = 5000,
        .validate_crc = true,
    };
    if (!saf_init(&s_engine, &ops, NULL, &cfg)) {
        ESP_LOGE(TAG, "engine init failed");
        return;
    }
    ESP_LOGI(TAG, "boot: depth=%u seq_next=%u",
             (unsigned)s_engine.depth, (unsigned)s_engine.seq_next);
    if (s_engine.st.recovered_torn > 0)
        printf("saf-recovered: dropped %u torn record\n",
               (unsigned)s_engine.st.recovered_torn);

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    shell_banner();

    int64_t next_sample_us = esp_timer_get_time();
    int32_t last_beat_s = 0;

    for (;;) {
        shell_poll();
        saf_tick(&s_engine);
#if CONFIG_SAF_TRANSPORT_FAKE
        fake_transport_poll(&s_engine);
#else
        mqtt_transport_poll(&s_engine);
#endif
        esp_task_wdt_reset();

        int64_t now_us = esp_timer_get_time();

        /* telemetry source: one reading every SAMPLE_MS, forever */
        if (now_us >= next_sample_us) {
            next_sample_us += (int64_t)SAMPLE_MS * 1000;
            char data[48];
            snprintf(data, sizeof data, "t=%u n=%u",
                     (unsigned)(now_us / 1000),
                     (unsigned)s_engine.st.enqueued);
            if (saf_enqueue(&s_engine, data, strlen(data))) {
                if (s_engine.link != SAF_LINK_UP)
                    printf("saf-queue: depth=%u seq=%u\n",
                           (unsigned)s_engine.depth,
                           (unsigned)(s_engine.seq_next - 1));
            } else {
                printf("saf-queue: DROPPED (depth=%u)\n",
                       (unsigned)s_engine.depth);
            }
        }

        /* the firmware-computed drain verdict */
        if (s_pending_drain) {
            if (s_engine.depth == 0 && !s_engine.inflight) {
                s_pending_drain = false;
                printf("saf: drained, nothing lost "
                       "(%u queued while out, acked=%u)\n",
                       (unsigned)s_drain_started_queued,
                       (unsigned)s_engine.st.acked);
            } else if (now_us > s_drain_deadline_us) {
                s_pending_drain = false;
                printf("saf DATA AT RISK: depth=%u still undelivered\n",
                       (unsigned)s_engine.depth);
            }
        }

        /* heartbeat every 5 s; CI waits for t=10s (past the 5 s WDT) */
        int32_t up_s = (int32_t)(now_us / 1000000);
        if (up_s >= last_beat_s + 5) {
            last_beat_s = up_s - (up_s % 5);
            ESP_LOGI(TAG, "alive t=%lds", (long)last_beat_s);
        }

        vTaskDelay(pdMS_TO_TICKS(LAP_MS));
    }
}
