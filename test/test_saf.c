/*
 * test_saf.c - host-native unit tests for main/saf_core.c.
 *
 * Everything the engine can touch is a scriptable in-memory mock, so
 * every failure the chaos suite later produces with real processes is
 * first pinned here deterministically: torn tail records, reboots
 * mid-outage, the ack-then-crash duplicate window, storage refusals,
 * stale acks, holes. Includes the unit-level torn-write CONTROL: the
 * same corrupt record must be ACCEPTED when validate_crc is off,
 * proving the detector - not luck - is what catches it.
 */
#include <stdio.h>
#include <string.h>

#include "../main/saf_core.h"

static int failures;
#define CHECK(cond) do {                                              \
        if (!(cond)) {                                                \
            failures++;                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                             \
    } while (0)

/* ------------------------------------------------------------ the mock -- */
#define MOCK_SLOTS 128

typedef struct {
    /* storage */
    struct {
        bool    present;
        uint8_t data[SAF_RECORD_MAX];
        size_t  len;
    } rec[MOCK_SLOTS];
    bool     meta_present;
    uint32_t meta;
    int32_t  fail_get_at;    /* report this idx missing; -1 = none */
    bool     fail_put;
    bool     fail_put_meta;
    bool     fail_erase;

    /* transport script */
    bool connect_ok;
    bool publish_ok;
    int  connects, disconnects, publishes;
    char     last_id[SAF_MSG_ID_MAX];
    uint32_t last_seq;
    bool     last_retry;
    uint8_t  last_payload[SAF_PAYLOAD_MAX];
    size_t   last_len;

    /* clock + jitter */
    uint32_t now;
    uint32_t rand_val;
} mock;

static bool m_put(void *ctx, uint32_t idx, const void *rec, size_t len)
{
    mock *m = ctx;
    if (m->fail_put)
        return false;
    m->rec[idx].present = true;
    memcpy(m->rec[idx].data, rec, len);
    m->rec[idx].len = len;
    return true;
}

static bool m_get(void *ctx, uint32_t idx, void *rec, size_t *len)
{
    mock *m = ctx;
    if ((int32_t)idx == m->fail_get_at || !m->rec[idx].present)
        return false;
    memcpy(rec, m->rec[idx].data, m->rec[idx].len);
    *len = m->rec[idx].len;
    return true;
}

static bool m_erase(void *ctx, uint32_t idx)
{
    mock *m = ctx;
    if (m->fail_erase)
        return false;
    m->rec[idx].present = false;
    return true;
}

static void m_scan(void *ctx, uint32_t *first, uint32_t *last, bool *any)
{
    mock *m = ctx;
    *any = false;
    for (uint32_t i = 0; i < MOCK_SLOTS; i++) {
        if (!m->rec[i].present)
            continue;
        if (!*any)
            *first = i;
        *last = i;
        *any = true;
    }
}

static bool m_get_meta(void *ctx, uint32_t *v)
{
    mock *m = ctx;
    if (!m->meta_present)
        return false;
    *v = m->meta;
    return true;
}

static bool m_put_meta(void *ctx, uint32_t v)
{
    mock *m = ctx;
    if (m->fail_put_meta)
        return false;
    m->meta_present = true;
    m->meta = v;
    return true;
}

static bool m_connect(void *ctx)
{
    mock *m = ctx;
    m->connects++;
    return m->connect_ok;
}

static void m_disconnect(void *ctx)
{
    mock *m = ctx;
    m->disconnects++;
}

static bool m_publish(void *ctx, const char *id, uint32_t seq,
                      const void *payload, size_t len, bool retry)
{
    mock *m = ctx;
    if (!m->publish_ok)
        return false;
    m->publishes++;
    strcpy(m->last_id, id);
    m->last_seq = seq;
    m->last_retry = retry;
    memcpy(m->last_payload, payload, len);
    m->last_len = len;
    return true;
}

static uint32_t m_now(void *ctx)   { return ((mock *)ctx)->now; }
static uint32_t m_rand(void *ctx)  { return ((mock *)ctx)->rand_val; }

static const saf_ops OPS = {
    m_put, m_get, m_erase, m_scan, m_get_meta, m_put_meta,
    m_connect, m_disconnect, m_publish, m_now, m_rand,
};

static const saf_config CFG = {
    .device_id = "dev01",
    .max_depth = 16,
    .ack_timeout_ms = 5000,
    .connect_timeout_ms = 3000,
    .validate_crc = true,
};

static void fresh(mock *m, saf *s, const saf_config *cfg)
{
    memset(m, 0, sizeof *m);
    m->fail_get_at = -1;
    m->connect_ok = true;
    m->publish_ok = true;
    CHECK(saf_init(s, &OPS, m, cfg ? cfg : &CFG));
}

/* Bring the link up the way a binding would: tick fires tr_connect,
 * then the transport reports CONNACK. */
static void bring_up(saf *s)
{
    saf_tick(s);
    saf_on_conn_up(s);
}

/* ------------------------------------------------------------- backoff -- */
static void test_backoff_arithmetic(void)
{
    /* growth: 500 << n until the 60 s cap */
    CHECK(saf_backoff_window(0) == 500);
    CHECK(saf_backoff_window(1) == 1000);
    CHECK(saf_backoff_window(6) == 32000);
    CHECK(saf_backoff_window(7) == 60000);   /* 64000 clipped to cap */
    CHECK(saf_backoff_window(8) == 60000);   /* exponent clamp */
    CHECK(saf_backoff_window(1000) == 60000);

    /* equal jitter: delay in [window/2, window) for any rand */
    CHECK(saf_backoff_delay(0, 0) == 250);
    CHECK(saf_backoff_delay(0, 249) == 499);
    CHECK(saf_backoff_delay(0, 250) == 250); /* modulo wraps */
    CHECK(saf_backoff_delay(7, 0xffffffffu) >= 30000);
    CHECK(saf_backoff_delay(7, 0xffffffffu) < 60000);
}

static void test_backoff_grows_and_resets(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    m.connect_ok = false;
    m.rand_val = 0; /* delay = window/2 exactly, deterministic */

    uint32_t expect_window = 500;
    for (int i = 0; i < 4; i++) {
        saf_tick(&s); /* fires connect, which fails */
        CHECK(s.link == SAF_LINK_DOWN);
        CHECK(s.backoff_until_ms == m.now + expect_window / 2);
        saf_tick(&s); /* still inside the backoff window: no attempt */
        CHECK(s.link == SAF_LINK_DOWN);
        m.now = s.backoff_until_ms;
        expect_window *= 2;
    }
    CHECK(m.connects == 4);

    /* a successful session resets the ladder */
    m.connect_ok = true;
    bring_up(&s);
    CHECK(s.attempt == 0);
    saf_on_conn_down(&s);
    CHECK(s.backoff_until_ms == m.now + 250); /* window back at 500 */

    /* the attempt counter clamps instead of wrapping */
    s.attempt = 1000;
    s.link = SAF_LINK_UP;
    saf_on_conn_down(&s);
    CHECK(s.attempt == 1000);
}

/* ------------------------------------------------------- happy delivery -- */
static void test_roundtrip_and_lease(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);

    CHECK(saf_enqueue(&s, "a", 1));
    CHECK(m.meta == SAF_SEQ_LEASE);       /* ceiling leased before seq 0 */
    CHECK(saf_enqueue(&s, "b", 1));
    CHECK(s.depth == 2);

    bring_up(&s);
    saf_tick(&s);
    CHECK(m.publishes == 1);
    CHECK(m.last_seq == 0);
    CHECK(!m.last_retry);
    CHECK(strcmp(m.last_id, "dev01-00000000") == 0);
    CHECK(m.last_len == 1 && m.last_payload[0] == 'a');

    /* window of one: nothing else goes out before the ack */
    saf_tick(&s);
    CHECK(m.publishes == 1);

    saf_on_puback(&s, 0);
    CHECK(s.depth == 1);
    CHECK(!m.rec[0].present);             /* erased only after the ack */
    saf_tick(&s);
    CHECK(m.last_seq == 1);
    saf_on_puback(&s, 1);
    CHECK(s.depth == 0 && s.st.acked == 2 && s.st.enqueued == 2);

    saf_tick(&s); /* empty queue: nothing to publish */
    CHECK(m.publishes == 2);
}

static void test_lease_renews_every_block(void)
{
    mock m; saf s;
    saf_config cfg = CFG;
    cfg.max_depth = 64;
    fresh(&m, &s, &cfg);

    for (uint32_t i = 0; i < SAF_SEQ_LEASE + 1; i++)
        CHECK(saf_enqueue(&s, "x", 1));
    CHECK(m.meta == 2 * SAF_SEQ_LEASE);   /* renewed at seq 32, not before */
}

/* ------------------------------------------------------ offline + limits -- */
static void test_queue_full_and_bad_input(void)
{
    mock m; saf s;
    saf_config cfg = CFG;
    cfg.max_depth = 2;
    fresh(&m, &s, &cfg);

    uint8_t big[SAF_PAYLOAD_MAX + 1] = {0};
    CHECK(!saf_enqueue(&s, big, sizeof big));

    CHECK(saf_enqueue(&s, "a", 1));
    CHECK(saf_enqueue(&s, "b", 1));
    CHECK(!saf_enqueue(&s, "c", 1));
    CHECK(s.st.dropped_full == 1);
    CHECK(s.depth == 2);
}

static void test_storage_refusals(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);

    m.fail_put_meta = true;
    CHECK(!saf_enqueue(&s, "a", 1));      /* no lease, no sequence issued */
    CHECK(s.seq_next == 0 && s.depth == 0);

    m.fail_put_meta = false;
    m.fail_put = true;
    CHECK(!saf_enqueue(&s, "a", 1));      /* lease ok, record refused */
    CHECK(s.depth == 0 && s.st.enqueued == 0);

    m.fail_put = false;
    CHECK(saf_enqueue(&s, "a", 1));
    bring_up(&s);
    saf_tick(&s);
    m.fail_erase = true;
    saf_on_puback(&s, 0);                 /* acked but the erase failed: */
    CHECK(s.st.erase_failures == 1);      /* duplicate-on-reboot, not loss */
    CHECK(s.depth == 0 && s.st.acked == 1);
}

/* --------------------------------------------------------- link failures -- */
static void test_ack_timeout_and_retry_flag(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    CHECK(saf_enqueue(&s, "a", 1));
    bring_up(&s);
    saf_tick(&s);
    CHECK(m.publishes == 1 && !m.last_retry);

    saf_tick(&s); /* fresh in-flight: no timeout yet */
    CHECK(m.disconnects == 0);

    m.now += CFG.ack_timeout_ms;
    saf_tick(&s); /* broker went quiet: drop the link, keep the record */
    CHECK(m.disconnects == 1);
    CHECK(s.link == SAF_LINK_DOWN && s.depth == 1);

    m.now = s.backoff_until_ms;
    bring_up(&s);
    saf_tick(&s);
    CHECK(m.publishes == 2);
    CHECK(m.last_retry);                  /* it MAY have reached the broker */
    CHECK(m.last_seq == 0);               /* same sequence, same msg id */
    saf_on_puback(&s, 0);
    CHECK(!s.inflight_retry && s.depth == 0);
}

static void test_connect_timeout_and_publish_failure(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);

    saf_tick(&s); /* connect accepted, no CONNACK yet */
    CHECK(s.link == SAF_LINK_CONNECTING);
    saf_tick(&s); /* still inside the window */
    CHECK(s.link == SAF_LINK_CONNECTING);
    m.now += CFG.connect_timeout_ms;
    saf_tick(&s);
    CHECK(s.link == SAF_LINK_DOWN && m.disconnects == 1);

    m.now = s.backoff_until_ms;
    CHECK(saf_enqueue(&s, "a", 1));
    bring_up(&s);
    m.publish_ok = false;
    saf_tick(&s); /* the write itself failed: that is a dead link too */
    CHECK(s.link == SAF_LINK_DOWN && m.disconnects == 2);
    CHECK(s.depth == 1);
}

static void test_callback_noise(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);

    saf_on_puback(&s, 7); /* nothing in flight */
    CHECK(s.st.stale_acks == 1);

    CHECK(saf_enqueue(&s, "a", 1));
    bring_up(&s);
    saf_tick(&s);
    saf_on_puback(&s, 99); /* ack for the wrong message */
    CHECK(s.st.stale_acks == 2 && s.depth == 1);

    saf_on_conn_up(&s); /* CONNACK glitch while in flight: re-arm as retry */
    CHECK(!s.inflight && s.inflight_retry);

    saf_on_conn_down(&s);
    uint32_t armed = s.backoff_until_ms;
    uint32_t attempts = s.attempt;
    saf_on_conn_down(&s); /* second notification while already down: noise */
    CHECK(s.backoff_until_ms == armed && s.attempt == attempts);
}

/* ------------------------------------------------------ reboot + recovery -- */
static void test_reboot_resumes_queue_and_sequence(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    for (int i = 0; i < 5; i++)
        CHECK(saf_enqueue(&s, "x", 1));
    bring_up(&s);
    saf_tick(&s);
    saf_on_puback(&s, 0);
    saf_tick(&s);
    saf_on_puback(&s, 1); /* two delivered, three still queued */

    saf s2; /* reboot: storage is the only memory */
    CHECK(saf_init(&s2, &OPS, &m, &CFG));
    CHECK(s2.depth == 3);
    CHECK(s2.head == 2 && s2.tail == 5);
    CHECK(s2.seq_next == SAF_SEQ_LEASE); /* resumes at the ceiling: */
    CHECK(saf_enqueue(&s2, "y", 1));     /* gaps allowed, reuse never */
    CHECK(s2.seq_next == SAF_SEQ_LEASE + 1);
}

static void test_crash_between_ack_and_erase_replays_same_id(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    CHECK(saf_enqueue(&s, "a", 1));
    bring_up(&s);
    saf_tick(&s);
    /* The broker got it (and PUBACKed), but power died before
     * saf_on_puback ran: the record is still in storage. */
    saf s2;
    CHECK(saf_init(&s2, &OPS, &m, &CFG));
    CHECK(s2.depth == 1);
    bring_up(&s2);
    saf_tick(&s2);
    CHECK(strcmp(m.last_id, "dev01-00000000") == 0); /* the SAME identity */
    /* ...which is the whole idempotency argument: downstream dedups it. */
}

static void test_seq_resumes_from_record_when_meta_lost(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    for (int i = 0; i < 3; i++)
        CHECK(saf_enqueue(&s, "x", 1));

    m.meta_present = false; /* the meta write lied / meta area lost */
    saf s2;
    CHECK(saf_init(&s2, &OPS, &m, &CFG));
    CHECK(s2.seq_next == 3); /* newest surviving record + 1 */
    CHECK(s2.seq_ceiling == 3);
}

static void test_device_id_too_long(void)
{
    mock m; saf s;
    memset(&m, 0, sizeof m);
    m.fail_get_at = -1;
    saf_config cfg = CFG;
    cfg.device_id = "sixteen-chars-xx"; /* 16: one over the limit */
    CHECK(!saf_init(&s, &OPS, &m, &cfg));
}

/* ---------------------------------------------------- torn-write handling -- */

/* A record whose payload half was never committed: header intact, tail
 * bytes stuck at 0xFF (how a cut mid-page-write reads back), length
 * unchanged - only the CRC can tell. */
static void plant_torn_record(mock *m, uint32_t idx, uint32_t seq)
{
    mock scratch;
    memset(&scratch, 0, sizeof scratch);
    saf tmp;
    saf_config cfg = CFG;
    scratch.fail_get_at = -1;
    CHECK(saf_init(&tmp, &OPS, &scratch, &cfg));
    tmp.seq_next = seq;
    tmp.seq_ceiling = seq + 1;
    CHECK(saf_enqueue(&tmp, "0123456789abcdef", 16));

    memcpy(&m->rec[idx], &scratch.rec[0], sizeof m->rec[idx]);
    for (size_t i = m->rec[idx].len / 2; i < m->rec[idx].len; i++)
        m->rec[idx].data[i] = 0xff;
    m->rec[idx].present = true;
}

static void test_torn_tail_detected_and_dropped(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    CHECK(saf_enqueue(&s, "good", 4));
    plant_torn_record(&m, 1, 1); /* the append the power cut interrupted */

    saf s2;
    CHECK(saf_init(&s2, &OPS, &m, &CFG));
    CHECK(s2.st.recovered_torn == 1);
    CHECK(s2.depth == 1);            /* the good record survives */
    CHECK(!m.rec[1].present);        /* the torn one is gone from storage */
}

/* THE CONTROL: the identical torn record must sail straight through
 * when the CRC check is stubbed out. If this test ever finds the torn
 * record rejected, the detector above is passing for some accidental
 * reason and its green means nothing (04's the-net-must-fire lesson). */
static void test_torn_tail_control_without_crc_check(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    CHECK(saf_enqueue(&s, "good", 4));
    plant_torn_record(&m, 1, 1);

    saf_config cfg = CFG;
    cfg.validate_crc = false;
    saf s2;
    CHECK(saf_init(&s2, &OPS, &m, &cfg));
    CHECK(s2.st.recovered_torn == 0); /* not detected... */
    CHECK(s2.depth == 2);             /* ...accepted as if healthy */
    bring_up(&s2);
    saf_tick(&s2);
    saf_on_puback(&s2, 0);
    saf_tick(&s2);                    /* and the garbage gets PUBLISHED */
    CHECK(m.publishes == 2);
    CHECK(m.last_payload[15] == 0xff);
}

/* A short write (record cut before the length field's worth of bytes
 * landed) is caught structurally, CRC or no CRC - defense in depth. */
static void test_short_record_rejected_even_without_crc(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    CHECK(saf_enqueue(&s, "0123456789abcdef", 16));
    m.rec[0].len /= 2; /* the file itself is short */

    saf_config cfg = CFG;
    cfg.validate_crc = false;
    saf s2;
    CHECK(saf_init(&s2, &OPS, &m, &cfg));
    CHECK(s2.st.recovered_torn == 1 && s2.depth == 0);
}

static void test_decode_rejects_each_corruption(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    CHECK(saf_enqueue(&s, "abcd", 4));

    /* bad magic */
    m.rec[0].data[0] ^= 0xff;
    saf s2;
    CHECK(saf_init(&s2, &OPS, &m, &CFG));
    CHECK(s2.st.recovered_torn == 1);
    m.rec[0].present = false;

    /* absurd length field (> SAF_PAYLOAD_MAX) */
    fresh(&m, &s, NULL);
    CHECK(saf_enqueue(&s, "abcd", 4));
    m.rec[0].data[8] = 0xff;
    m.rec[0].data[9] = 0xff;
    CHECK(saf_init(&s2, &OPS, &m, &CFG));
    CHECK(s2.st.recovered_torn == 1);

    /* truncated below any possible record */
    fresh(&m, &s, NULL);
    CHECK(saf_enqueue(&s, "abcd", 4));
    m.rec[0].len = 3;
    CHECK(saf_init(&s2, &OPS, &m, &CFG));
    CHECK(s2.st.recovered_torn == 1);
}

/* ------------------------------------------------------- holes mid-queue -- */
static void test_hole_and_interior_corruption_skipped_in_flight(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    for (int i = 0; i < 3; i++)
        CHECK(saf_enqueue(&s, "x", 1));

    /* head record unreadable (a half-failed erase from a past life) */
    m.fail_get_at = 0;
    bring_up(&s);
    saf_tick(&s);
    CHECK(m.publishes == 0 && s.depth == 2 && s.head == 1);

    /* next record corrupt: dropped in flight, counted */
    m.fail_get_at = -1;
    m.rec[1].data[0] ^= 0xff;
    saf_tick(&s);
    CHECK(m.publishes == 0 && s.depth == 1 && s.st.recovered_torn == 1);
    CHECK(!m.rec[1].present);

    /* the survivor still goes out */
    saf_tick(&s);
    CHECK(m.publishes == 1 && m.last_seq == 2);
}

/* Recovery must also trim a tail key the store lists but cannot read
 * back (storage's own recovery dropped the entry under our feet). */
static void test_recovery_trims_unreadable_tail(void)
{
    mock m; saf s;
    fresh(&m, &s, NULL);
    for (int i = 0; i < 3; i++)
        CHECK(saf_enqueue(&s, "x", 1));
    m.fail_get_at = 2; /* scanned as present, unreadable on get */

    saf s2;
    CHECK(saf_init(&s2, &OPS, &m, &CFG));
    CHECK(s2.head == 0 && s2.tail == 2 && s2.depth == 2);
    CHECK(s2.st.recovered_torn == 0); /* trims are not corruption */
}

/* ------------------------------------------------------------------ main -- */
int main(void)
{
    test_backoff_arithmetic();
    test_backoff_grows_and_resets();
    test_roundtrip_and_lease();
    test_lease_renews_every_block();
    test_queue_full_and_bad_input();
    test_storage_refusals();
    test_ack_timeout_and_retry_flag();
    test_connect_timeout_and_publish_failure();
    test_callback_noise();
    test_reboot_resumes_queue_and_sequence();
    test_crash_between_ack_and_erase_replays_same_id();
    test_seq_resumes_from_record_when_meta_lost();
    test_device_id_too_long();
    test_torn_tail_detected_and_dropped();
    test_torn_tail_control_without_crc_check();
    test_short_record_rejected_even_without_crc();
    test_decode_rejects_each_corruption();
    test_hole_and_interior_corruption_skipped_in_flight();
    test_recovery_trims_unreadable_tail();

    if (failures) {
        printf("test_saf: %d FAILURE%s\n", failures, failures > 1 ? "S" : "");
        return 1;
    }
    printf("test_saf: all checks pass\n");
    return 0;
}
