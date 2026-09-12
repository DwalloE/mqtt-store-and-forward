/*
 * mqtt_mini.h - a deliberately small MQTT 3.1.1 client over plain TCP,
 * just enough for the host device: CONNECT with a controllable
 * clean-session flag, QoS 1 PUBLISH with the DUP bit, PUBACK receipt.
 *
 * Why hand-rolled: the DEVICE side of the chaos suite must be the same
 * saf_core the firmware runs, bound to a transport we can reason about
 * byte-by-byte (the proxy cuts frames mid-flight; knowing exactly what
 * was on the wire is the point). The SUBSCRIBER side deliberately uses
 * an independent, standard client (paho) - the verdict must not be
 * computed by the code under test.
 *
 * Omissions, all deliberate: no TLS, no QoS 2 (docs/protocol-choice.md
 * argues why the design doesn't want it), no SUBSCRIBE, no PINGREQ
 * (every chaos scenario finishes well inside the 60 s keepalive).
 */
#ifndef MQTT_MINI_H
#define MQTT_MINI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int  fd;                /* -1 when closed */
    bool session_present;   /* from the last CONNACK */
    uint8_t rx[600];        /* partial-frame accumulator */
    size_t  rx_len;
} mqtt_mini;

/* TCP connect + MQTT CONNECT + wait for CONNACK (bounded by timeout_ms).
 * Returns true only on CONNACK rc=0; mm->session_present is then valid. */
bool mm_connect(mqtt_mini *mm, const char *host, int port,
                const char *client_id, bool clean_session,
                uint16_t keepalive_s, int timeout_ms);

/* QoS 1 PUBLISH. pid must be non-zero. */
bool mm_publish_qos1(mqtt_mini *mm, const char *topic,
                     const void *payload, size_t len,
                     uint16_t pid, bool dup);

/* Pump the socket for up to timeout_ms.
 * Returns 1 with *puback_pid set when a PUBACK arrived, 0 when nothing
 * relevant happened, -1 when the connection is dead. */
int mm_poll(mqtt_mini *mm, int timeout_ms, uint16_t *puback_pid);

void mm_close(mqtt_mini *mm);

#endif /* MQTT_MINI_H */
