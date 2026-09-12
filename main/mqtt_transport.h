/*
 * mqtt_transport.h - the real binding: esp-mqtt over Wi-Fi, QoS 1,
 * clean_session=false, auto-reconnect OFF because the engine owns the
 * backoff. Compiled in every build so `idf.py build` proves it; started
 * only under CONFIG_SAF_TRANSPORT_REAL (the Wokwi CI scenario runs the
 * fake - wokwi-cli has no IoT gateway).
 *
 * esp-mqtt's events arrive on its own task; they are queued here and
 * drained into the engine from the supervisor loop, so saf_core is
 * only ever touched from one context.
 */
#ifndef MQTT_TRANSPORT_H
#define MQTT_TRANSPORT_H

#include "saf_core.h"

/* Connects Wi-Fi (blocking, from Kconfig credentials), creates the
 * mqtt client and fills the transport slots of *ops. */
void mqtt_transport_init(saf_ops *ops, const char *device_id);

/* Drain queued CONNECTED/DISCONNECTED/PUBACK events into the engine;
 * call from the same loop as saf_tick. */
void mqtt_transport_poll(saf *s);

#endif /* MQTT_TRANSPORT_H */
