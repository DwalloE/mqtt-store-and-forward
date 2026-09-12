/*
 * fake_transport.h - the injected transport that lies on command.
 *
 * `out` makes the radio "fail": connects are refused, an established
 * session drops. `in` restores it. The engine cannot tell this from a
 * real outage - same callbacks, same timing shape (CONNACK after a
 * short delay, PUBACK after a short delay) - which is what makes the
 * Wokwi scenario deterministic: wokwi-cli has no IoT gateway (decided
 * day one, see README honest limits), so the on-target proof runs
 * against this transport while every real-broker byte is exercised
 * host-side by the chaos suite.
 */
#ifndef FAKE_TRANSPORT_H
#define FAKE_TRANSPORT_H

#include "saf_core.h"

/* Fills the transport slots of *ops. */
void fake_transport_init(saf_ops *ops);

/* Deliver due CONNACKs/PUBACKs into the engine; call from the same
 * loop as saf_tick. */
void fake_transport_poll(saf *s);

/* The shell's `out` / `in`. Needs the engine to report the drop. */
void fake_transport_set_online(saf *s, bool online);
bool fake_transport_online(void);

#endif /* FAKE_TRANSPORT_H */
