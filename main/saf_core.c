/*
 * saf_core.c - the store-and-forward engine. Pure C, no IDF includes;
 * see saf_core.h for the contract and the delivery model.
 *
 * Invariants the tests pin:
 *   - depth == tail - head, records live at keys [head, tail).
 *   - Nothing is ever reported enqueued unless store_put succeeded.
 *   - A record leaves storage only after its PUBACK (or after failing
 *     validation) - so every crash window either replays (duplicate,
 *     absorbed downstream by the idempotent ID) or loses nothing.
 *   - seq_next < seq_ceiling whenever a sequence is issued: the ceiling
 *     lease is persisted BEFORE the first sequence of each lease block,
 *     so a reboot can skip numbers but can never reuse one.
 */
#include "saf_core.h"

#include <string.h>

/* Record layout, little-endian, packed by hand so host and target agree:
 *   [0]  u32 magic  [4] u32 seq  [8] u16 len  [10] payload  [10+len] u32 crc
 * crc covers bytes [0, 10+len). */
#define REC_MAGIC 0x53414651u /* "QFAS" on the wire, saf-queue */
#define REC_HDR   10u

/* ------------------------------------------------------------ encoding -- */
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static size_t rec_encode(uint8_t *rec, uint32_t seq,
                         const void *payload, size_t len)
{
    wr32(rec, REC_MAGIC);
    wr32(rec + 4, seq);
    rec[8] = (uint8_t)len;
    rec[9] = (uint8_t)(len >> 8);
    memcpy(rec + REC_HDR, payload, len);
    wr32(rec + REC_HDR + len, crc32_of(rec, REC_HDR + len));
    return REC_HDR + len + 4;
}

/* The torn-write detector. A power cut mid-append leaves a short or
 * garbage record; every failure mode lands in one of these checks. The
 * CRC check is the one the control build (validate_crc=false) disables. */
static bool rec_decode(const saf *s, const uint8_t *rec, size_t size,
                       uint32_t *seq, const uint8_t **payload, size_t *len)
{
    if (size < REC_HDR + 4)
        return false;
    if (rd32(rec) != REC_MAGIC)
        return false;
    size_t plen = (size_t)rec[8] | ((size_t)rec[9] << 8);
    if (plen > SAF_PAYLOAD_MAX)
        return false;
    if (size != REC_HDR + plen + 4)
        return false;
    if (s->cfg.validate_crc &&
        rd32(rec + REC_HDR + plen) != crc32_of(rec, REC_HDR + plen))
        return false;
    *seq = rd32(rec + 4);
    *payload = rec + REC_HDR;
    *len = plen;
    return true;
}

/* ------------------------------------------------------------- backoff -- */
uint32_t saf_backoff_window(uint32_t attempt)
{
    /* 500 << 7 already clears the 60 s cap; clamping the exponent keeps
     * the shift defined for any attempt count. */
    uint32_t exp = attempt > 7u ? 7u : attempt;
    uint32_t window = SAF_BACKOFF_BASE_MS << exp;
    return window > SAF_BACKOFF_CAP_MS ? SAF_BACKOFF_CAP_MS : window;
}

uint32_t saf_backoff_delay(uint32_t attempt, uint32_t r)
{
    /* Equal jitter: [window/2, window). Never zero, so a dead broker is
     * never hammered; never above the window, so recovery is prompt. */
    uint32_t w = saf_backoff_window(attempt);
    return w / 2u + r % (w / 2u);
}

static void schedule_backoff(saf *s)
{
    uint32_t delay = saf_backoff_delay(s->attempt, s->ops.rand32(s->ctx));
    s->backoff_until_ms = s->ops.now_ms(s->ctx) + delay;
    if (s->attempt < 1000u) /* enough for any cap math, never wraps */
        s->attempt++;
}

/* The one exit path from an established or half-established link. An
 * in-flight message survives as head-of-queue and is re-published with
 * the retry flag - it MAY have reached the broker, which is exactly the
 * duplicate the idempotent IDs exist for. */
static void link_lost(saf *s)
{
    if (s->inflight) {
        s->inflight = false;
        s->inflight_retry = true;
    }
    s->link = SAF_LINK_DOWN;
    schedule_backoff(s);
}

/* ---------------------------------------------------------------- init -- */
bool saf_init(saf *s, const saf_ops *ops, void *ctx, const saf_config *cfg)
{
    memset(s, 0, sizeof *s);
    s->ops = *ops;
    s->ctx = ctx;
    s->cfg = *cfg;
    if (strlen(cfg->device_id) >= SAF_DEVICE_ID_MAX)
        return false;

    /* Rebuild the queue window from whatever survived the last life. */
    uint32_t first, last;
    bool any;
    s->ops.store_scan(s->ctx, &first, &last, &any);
    if (any) {
        s->head = first;
        s->tail = last + 1u;
    }

    /* Trim torn records off the tail - the only place a power cut can
     * tear (appends happen at tail). Anything invalid is erased and
     * counted; a missing key is just trimmed. The newest surviving
     * record's sequence is captured for the resume arithmetic below. */
    uint8_t rec[SAF_RECORD_MAX];
    bool have_newest = false;
    uint32_t newest_seq = 0;
    while (s->tail > s->head) {
        size_t size = sizeof rec;
        uint32_t seq;
        const uint8_t *payload;
        size_t plen;
        if (!s->ops.store_get(s->ctx, s->tail - 1u, rec, &size)) {
            s->tail--;
            continue;
        }
        if (rec_decode(s, rec, size, &seq, &payload, &plen)) {
            have_newest = true;
            newest_seq = seq;
            break;
        }
        s->ops.store_erase(s->ctx, s->tail - 1u);
        s->st.recovered_torn++;
        s->tail--;
    }
    s->depth = s->tail - s->head;

    /* Sequence resume: the ceiling lease guarantees no reuse; the newest
     * surviving record guards against a meta store that lied. */
    if (!s->ops.store_get_meta(s->ctx, &s->seq_ceiling))
        s->seq_ceiling = 0;
    s->seq_next = s->seq_ceiling;
    if (have_newest && newest_seq >= s->seq_next) {
        s->seq_next = newest_seq + 1u;
        s->seq_ceiling = s->seq_next;
    }

    /* First connect attempt fires on the first tick. */
    s->link = SAF_LINK_DOWN;
    s->backoff_until_ms = s->ops.now_ms(s->ctx);
    return true;
}

/* ------------------------------------------------------------- enqueue -- */
bool saf_enqueue(saf *s, const void *payload, size_t len)
{
    if (len > SAF_PAYLOAD_MAX)
        return false;
    if (s->depth >= s->cfg.max_depth) {
        s->st.dropped_full++;
        return false;
    }

    /* Lease a new ceiling BEFORE issuing the first sequence past the old
     * one - the invariant that makes reboot-resume collision-free. */
    if (s->seq_next >= s->seq_ceiling) {
        if (!s->ops.store_put_meta(s->ctx, s->seq_ceiling + SAF_SEQ_LEASE))
            return false;
        s->seq_ceiling += SAF_SEQ_LEASE;
    }

    uint8_t rec[SAF_RECORD_MAX];
    size_t size = rec_encode(rec, s->seq_next, payload, len);
    if (!s->ops.store_put(s->ctx, s->tail, rec, size))
        return false;

    s->tail++;
    s->depth++;
    s->seq_next++;
    s->st.enqueued++;
    return true;
}

/* ---------------------------------------------------------------- tick -- */
void saf_tick(saf *s)
{
    uint32_t now = s->ops.now_ms(s->ctx);

    if (s->link == SAF_LINK_DOWN) {
        if ((int32_t)(now - s->backoff_until_ms) >= 0) {
            s->link = SAF_LINK_CONNECTING;
            s->connect_started_ms = now;
            if (!s->ops.tr_connect(s->ctx))
                link_lost(s);
        }
        return;
    }

    if (s->link == SAF_LINK_CONNECTING) {
        if (now - s->connect_started_ms >= s->cfg.connect_timeout_ms) {
            s->ops.tr_disconnect(s->ctx);
            link_lost(s);
        }
        return;
    }

    /* SAF_LINK_UP */
    if (s->inflight) {
        if (now - s->inflight_sent_ms >= s->cfg.ack_timeout_ms) {
            /* The broker went quiet mid-conversation; the message
             * stays queued and goes out again as a retry. */
            s->ops.tr_disconnect(s->ctx);
            link_lost(s);
        }
        return;
    }
    if (s->depth == 0)
        return;

    uint8_t rec[SAF_RECORD_MAX];
    size_t size = sizeof rec;
    uint32_t seq;
    const uint8_t *payload;
    size_t plen;
    if (!s->ops.store_get(s->ctx, s->head, rec, &size)) {
        /* A hole (e.g. an erase that failed halfway once): skip it. */
        s->head++;
        s->depth--;
        return;
    }
    if (!rec_decode(s, rec, size, &seq, &payload, &plen)) {
        s->ops.store_erase(s->ctx, s->head);
        s->st.recovered_torn++;
        s->head++;
        s->depth--;
        return;
    }

    char id[SAF_MSG_ID_MAX];
    saf_msg_id(s, seq, id);
    if (!s->ops.tr_publish(s->ctx, id, seq, payload, plen,
                           s->inflight_retry)) {
        s->ops.tr_disconnect(s->ctx);
        link_lost(s);
        return;
    }
    s->inflight = true;
    s->inflight_seq = seq;
    s->inflight_sent_ms = now;
    s->st.sent++;
}

/* ------------------------------------------------------------ callbacks -- */
void saf_on_conn_up(saf *s)
{
    s->link = SAF_LINK_UP;
    s->attempt = 0; /* backoff resets on success */
    if (s->inflight) {
        /* Connected while a publish was pending from a previous session
         * (shouldn't happen with a well-behaved transport, but a callback
         * ordering glitch must not strand the message). */
        s->inflight = false;
        s->inflight_retry = true;
    }
}

void saf_on_conn_down(saf *s)
{
    if (s->link == SAF_LINK_DOWN)
        return; /* already backing off; a second notification is noise */
    link_lost(s);
}

void saf_on_puback(saf *s, uint32_t seq)
{
    if (!s->inflight || seq != s->inflight_seq) {
        s->st.stale_acks++;
        return;
    }
    /* Erase-after-ack. If the erase fails the record stays: it will be
     * re-published after the next boot as a duplicate, and the
     * idempotent ID lets downstream drop it - loss is the only
     * unforgivable outcome here, duplicates are the design. */
    if (!s->ops.store_erase(s->ctx, s->head))
        s->st.erase_failures++;
    s->head++;
    s->depth--;
    s->inflight = false;
    s->inflight_retry = false;
    s->st.acked++;
}

/* -------------------------------------------------------------- msg id -- */
void saf_msg_id(const saf *s, uint32_t seq, char out[SAF_MSG_ID_MAX])
{
    /* hand-rolled "%s-%08u" - the engine stays printf-free */
    size_t n = strlen(s->cfg.device_id);
    memcpy(out, s->cfg.device_id, n);
    out[n] = '-';
    for (int i = 7; i >= 0; i--) {
        out[n + 1 + (size_t)i] = (char)('0' + seq % 10u);
        seq /= 10u;
    }
    out[n + 9] = '\0';
}
