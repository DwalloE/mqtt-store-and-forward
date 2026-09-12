/*
 * mqtt_mini.c - see mqtt_mini.h. MQTT 3.1.1 (protocol level 4).
 */
#define _POSIX_C_SOURCE 200809L

#include "mqtt_mini.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------- helpers -- */
static bool send_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Remaining-length varint; our frames are tiny but encode it properly. */
static size_t put_remlen(uint8_t *out, size_t len)
{
    size_t n = 0;
    do {
        uint8_t d = len % 128u;
        len /= 128u;
        out[n++] = d | (len > 0 ? 0x80u : 0u);
    } while (len > 0);
    return n;
}

static void put_u16(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v >> 8);
    out[1] = (uint8_t)v;
}

/* ------------------------------------------------------------- connect -- */
bool mm_connect(mqtt_mini *mm, const char *host, int port,
                const char *client_id, bool clean_session,
                uint16_t keepalive_s, int timeout_ms)
{
    mm->fd = -1;
    mm->rx_len = 0;
    mm->session_present = false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return false;
    }

    /* CONNECT: "MQTT" level 4, flags carry the clean-session choice -
     * we send 0 there, so the broker keeps QoS1 state across our
     * disconnects (session resumption; CONNACK echoes what it kept). */
    size_t idlen = strlen(client_id);
    uint8_t var[10 + 2 + 64];
    size_t v = 0;
    memcpy(var + v, "\x00\x04MQTT\x04", 7); v += 7;
    var[v++] = clean_session ? 0x02 : 0x00;
    put_u16(var + v, keepalive_s); v += 2;
    put_u16(var + v, (uint16_t)idlen); v += 2;
    memcpy(var + v, client_id, idlen); v += idlen;

    uint8_t frame[16 + sizeof var];
    size_t n = 0;
    frame[n++] = 0x10;
    n += put_remlen(frame + n, v);
    memcpy(frame + n, var, v); n += v;
    if (!send_all(fd, frame, n)) {
        close(fd);
        return false;
    }

    /* CONNACK: 0x20 0x02 <ack-flags> <return-code> */
    uint8_t ack[4];
    size_t got = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    while (got < 4) {
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            close(fd);
            return false;
        }
        ssize_t r = recv(fd, ack + got, 4 - got, 0);
        if (r <= 0) {
            close(fd);
            return false;
        }
        got += (size_t)r;
    }
    if (ack[0] != 0x20 || ack[1] != 0x02 || ack[3] != 0x00) {
        close(fd);
        return false;
    }
    mm->session_present = (ack[2] & 0x01) != 0;
    mm->fd = fd;
    return true;
}

/* ------------------------------------------------------------- publish -- */
bool mm_publish_qos1(mqtt_mini *mm, const char *topic,
                     const void *payload, size_t len,
                     uint16_t pid, bool dup)
{
    if (mm->fd < 0)
        return false;
    size_t tlen = strlen(topic);
    size_t rem = 2 + tlen + 2 + len;

    uint8_t frame[8 + 2 + 128 + 2 + 512];
    size_t n = 0;
    frame[n++] = (uint8_t)(0x32 | (dup ? 0x08 : 0x00)); /* QoS1 [+DUP] */
    n += put_remlen(frame + n, rem);
    put_u16(frame + n, (uint16_t)tlen); n += 2;
    memcpy(frame + n, topic, tlen); n += tlen;
    put_u16(frame + n, pid); n += 2;
    memcpy(frame + n, payload, len); n += len;
    return send_all(mm->fd, frame, n);
}

/* ---------------------------------------------------------------- poll -- */
/* Frame boundary scan over the accumulator; consumes complete frames,
 * returns on the first PUBACK. Anything else the broker sends (there
 * should be nothing - we never SUBSCRIBE) is discarded frame-wise. */
static int scan_frames(mqtt_mini *mm, uint16_t *puback_pid)
{
    for (;;) {
        if (mm->rx_len < 2)
            return 0;
        size_t rem = 0, mult = 1, i = 1;
        for (;;) {
            if (i >= mm->rx_len)
                return 0; /* varint incomplete */
            uint8_t d = mm->rx[i++];
            rem += (size_t)(d & 0x7f) * mult;
            if (!(d & 0x80))
                break;
            mult *= 128;
        }
        size_t frame_len = i + rem;
        if (frame_len > sizeof mm->rx)
            return -1; /* nothing legitimate is this big here */
        if (mm->rx_len < frame_len)
            return 0;

        uint8_t type = mm->rx[0] >> 4;
        bool is_puback = type == 4 && rem >= 2;
        uint16_t pid = 0;
        if (is_puback)
            pid = (uint16_t)((mm->rx[i] << 8) | mm->rx[i + 1]);

        memmove(mm->rx, mm->rx + frame_len, mm->rx_len - frame_len);
        mm->rx_len -= frame_len;

        if (is_puback) {
            *puback_pid = pid;
            return 1;
        }
    }
}

int mm_poll(mqtt_mini *mm, int timeout_ms, uint16_t *puback_pid)
{
    if (mm->fd < 0)
        return -1;

    int r = scan_frames(mm, puback_pid);
    if (r != 0)
        return r;

    struct pollfd pfd = { .fd = mm->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0)
        return -1;
    if (pr == 0)
        return 0;

    ssize_t n = recv(mm->fd, mm->rx + mm->rx_len,
                     sizeof mm->rx - mm->rx_len, 0);
    if (n <= 0)
        return -1; /* peer closed or RST: the proxy's scissors */
    mm->rx_len += (size_t)n;
    return scan_frames(mm, puback_pid);
}

void mm_close(mqtt_mini *mm)
{
    if (mm->fd >= 0)
        close(mm->fd);
    mm->fd = -1;
}
