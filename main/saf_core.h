/*
 * saf_core.h - store-and-forward engine, pure C.
 *
 * No ESP-IDF includes anywhere in this pair of files (03's injected-bus
 * discipline). The engine owns the queue window, the sequence numbers,
 * the backoff arithmetic and the delivery state machine; everything that
 * touches the outside world - storage, transport, clock, randomness -
 * arrives through the saf_ops table. The host chaos suite binds these to
 * a file-backed store and a real TCP MQTT client; the firmware binds the
 * same file to NVS and esp-mqtt. Neither side is special: the engine
 * cannot tell a mosquitto SIGKILL from a Wi-Fi dropout, which is the
 * point.
 *
 * Delivery model: at-least-once with idempotent message IDs. One message
 * in flight (QoS 1), erase-after-PUBACK, so a power cut between "acked"
 * and "erased" replays the same <device>-<seq> ID on the next boot and
 * the subscriber's dedup absorbs it. docs/protocol-choice.md argues why
 * this beats QoS 2 here.
 */
#ifndef SAF_CORE_H
#define SAF_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Payload is the application's telemetry bytes; the record adds a
 * 10-byte header (magic, seq, len) and a trailing CRC32. */
#define SAF_PAYLOAD_MAX   96u
#define SAF_RECORD_OVERHEAD 14u
#define SAF_RECORD_MAX    (SAF_PAYLOAD_MAX + SAF_RECORD_OVERHEAD)

/* "<device>-<8-digit seq>" plus terminator. */
#define SAF_DEVICE_ID_MAX 16u
#define SAF_MSG_ID_MAX    (SAF_DEVICE_ID_MAX + 1u + 8u + 1u)

/* Sequence-ceiling lease: the meta record is rewritten once per this
 * many messages, not once per message (docs/nvs-queue.md does the
 * erase-cycle arithmetic on it). */
#define SAF_SEQ_LEASE     32u

/* Backoff: 500 ms doubling to a 60 s cap, equal jitter. */
#define SAF_BACKOFF_BASE_MS 500u
#define SAF_BACKOFF_CAP_MS  60000u

/* Everything the engine may do to the world. Every callback takes the
 * bound context first; storage indices are the engine's forever-advancing
 * record keys ([head, tail) is the live window). */
typedef struct {
    /* storage - one record per advancing key */
    bool (*store_put)(void *ctx, uint32_t idx, const void *rec, size_t len);
    /* Returns false if no record at idx. *len is in/out: capacity in,
     * stored size out. */
    bool (*store_get)(void *ctx, uint32_t idx, void *rec, size_t *len);
    bool (*store_erase)(void *ctx, uint32_t idx);
    /* Lowest and highest key present; *any=false when the store is empty. */
    void (*store_scan)(void *ctx, uint32_t *first, uint32_t *last, bool *any);
    /* The sequence-ceiling meta record. get returns false when absent. */
    bool (*store_get_meta)(void *ctx, uint32_t *seq_ceiling);
    bool (*store_put_meta)(void *ctx, uint32_t seq_ceiling);

    /* transport - all asynchronous; outcomes come back through
     * saf_on_conn_up / saf_on_conn_down / saf_on_puback */
    bool (*tr_connect)(void *ctx);
    void (*tr_disconnect)(void *ctx);
    bool (*tr_publish)(void *ctx, const char *msg_id, uint32_t seq,
                       const void *payload, size_t len, bool retry);

    /* clock + jitter source */
    uint32_t (*now_ms)(void *ctx);
    uint32_t (*rand32)(void *ctx);
} saf_ops;

typedef struct {
    const char *device_id;      /* <= SAF_DEVICE_ID_MAX-1 chars */
    uint32_t max_depth;         /* queue cap; enqueue beyond it drops */
    uint32_t ack_timeout_ms;    /* unacked publish older than this = link bad */
    uint32_t connect_timeout_ms;
    /* The torn-write safety net. false is the CONTROL configuration: the
     * chaos suite proves detection FAILS without it before trusting the
     * pass with it (04's the-net-must-fire lesson). */
    bool validate_crc;
} saf_config;

typedef enum {
    SAF_LINK_DOWN = 0,          /* waiting out the backoff window */
    SAF_LINK_CONNECTING,
    SAF_LINK_UP,
} saf_link_state;

typedef struct {
    uint32_t enqueued;          /* accepted and durably stored, this boot */
    uint32_t sent;              /* publish attempts (retries included) */
    uint32_t acked;             /* PUBACKs matched to the in-flight seq */
    uint32_t dropped_full;      /* enqueue refused: queue at max_depth */
    uint32_t recovered_torn;    /* invalid records dropped at recovery */
    uint32_t erase_failures;    /* acked but not erased: duplicate on reboot */
    uint32_t stale_acks;        /* PUBACK for something not in flight */
} saf_stats;

typedef struct {
    saf_ops  ops;
    void    *ctx;
    saf_config cfg;

    /* live queue window: records at keys [head, tail), depth = tail-head */
    uint32_t head, tail, depth;

    /* sequence issue state */
    uint32_t seq_next, seq_ceiling;

    /* link + backoff */
    saf_link_state link;
    uint32_t attempt;           /* consecutive failures, resets on conn up */
    uint32_t backoff_until_ms;
    uint32_t connect_started_ms;

    /* the single in-flight message */
    bool     inflight;
    bool     inflight_retry;
    uint32_t inflight_seq;
    uint32_t inflight_sent_ms;

    saf_stats st;
} saf;

/* Recovers the queue from storage (dropping torn records when
 * cfg.validate_crc), restores the sequence from the ceiling lease, and
 * arms an immediate first connect attempt. Returns false only on a
 * device_id that does not fit. */
bool saf_init(saf *s, const saf_ops *ops, void *ctx, const saf_config *cfg);

/* Persist payload (assigning the next sequence number) for eventual
 * delivery. Returns false and touches nothing durable on queue-full,
 * oversized payload, or storage refusal. */
bool saf_enqueue(saf *s, const void *payload, size_t len);

/* Drive the state machine; call often (every few ms is fine, it is all
 * deadline checks). */
void saf_tick(saf *s);

/* Transport outcome callbacks. */
void saf_on_conn_up(saf *s);
void saf_on_conn_down(saf *s);
void saf_on_puback(saf *s, uint32_t seq);

/* "<device>-%08u" - the idempotent identity a downstream dedup keys on. */
void saf_msg_id(const saf *s, uint32_t seq, char out[SAF_MSG_ID_MAX]);

/* Backoff arithmetic, exposed pure for the unit tests: window(attempt) is
 * min(cap, base << attempt); delay picks equal-jittered inside it,
 * [window/2, window). */
uint32_t saf_backoff_window(uint32_t attempt);
uint32_t saf_backoff_delay(uint32_t attempt, uint32_t r);

#ifdef __cplusplus
}
#endif

#endif /* SAF_CORE_H */
