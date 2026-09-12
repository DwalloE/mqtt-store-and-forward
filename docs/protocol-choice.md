# Protocol choice: MQTT vs CoAP vs raw TCP vs LoRaWAN, with the arithmetic

The workload: a battery-adjacent ESP32 publishing ~40-byte telemetry readings
(wrapped to a ~100-byte wire payload with its idempotent identity), cadence
anywhere from every few seconds to hourly, over connectivity that is *expected*
to fail. Requirements: no loss, no effective duplicates, fleet fan-out to
multiple consumers, and an ops story a small team can run. Project 08 inherits
the power numbers below.

Labels used throughout: **[datasheet]** = from a vendor datasheet,
**[computed]** = arithmetic on stated assumptions, **[assumed]** = engineering
estimate this repo cannot measure without a bench.

## Bytes on the wire per message [computed]

Wire payload: `{"id":"esp01-00000042","seq":42,"data":"t=1234 n=42"}` ≈ 55-70 B;
call it 64 B. Topic `saf/esp01/telemetry` = 19 B.

| Stack | Per-message bytes (both directions, steady state) |
|---|---|
| MQTT QoS1 / TCP | PUBLISH: 2 fixed + 21 topic+len + 2 pid + 64 = 89 B + 40 B TCP/IP header; PUBACK 4 B + 40; two TCP ACKs ~80 -> **~253 B** |
| + TLS 1.2 | + ~29 B/record + ~5 KB handshake amortized over the session **[assumed]** |
| CoAP CON / UDP | ~4 B header + ~8 B options + 64 = 76 B + 28 B UDP/IP; ACK ~12+28 -> **~144 B** (~43% less) |
| Raw TCP framing | ~70 B + 40 + ack 40 -> **~150 B** - but the ack/retry/dedup protocol you then hand-roll is... this repo's engine, without the ecosystem |
| LoRaWAN | 13 B MAC overhead + payload; **but** the 89 B application frame exceeds the 51 B SF11/SF12 limit -> fragmentation at exactly the spreading factors bad links force you onto |

Byte counts alone say CoAP. Bytes are not the requirement.

## Duty cycle: where LoRaWAN actually lands [computed]

EU868 imposes 1% duty cycle (and TTN's fair-use policy is 30 s airtime/day).
Airtime for a 64-byte application payload: ~118 ms at SF7, ~2.6 s at SF12
(fragmented). At 1% duty cycle that caps a single device at ~305 msgs/hour
(SF7, good link) collapsing to ~13/hour at SF12 - and the fleet shares the
gateway's downlink budget, which QoS-style confirmed traffic devours: TTN's
fair use allows **10 confirmed downlinks per day**. A store-and-forward design
that *acknowledges every message* is architecturally wrong on LoRaWAN; you
redesign around unconfirmed uplinks + application-layer batch acks. LoRaWAN is
the right answer for a different question (years on a coin cell, km of range,
bytes per day). It is not this question.

## Power per message, ESP32 Wi-Fi vs LoRa [computed from datasheet currents]

Currents: ESP32 Wi-Fi TX ~240 mA peak / ~120 mA averaged over an active
connect-publish burst **[datasheet, ESP32 series]**; deep sleep 10 uA
[datasheet]; SX1276 TX at +14 dBm ~29 mA [datasheet].

Duty-cycled Wi-Fi publish (wake -> associate -> DHCP -> TCP+MQTT connect ->
publish -> sleep): 2.5 s at ~120 mA **[assumed burst profile]** ->
0.083 mAh/message.

- Hourly cadence: 24 x 0.083 + 0.24 (sleep) ≈ **2.2 mAh/day** -> ~2.4 years on
  a 2000 mAh cell [computed].
- 10-minute cadence: **~12 mAh/day** -> ~5.5 months [computed].
- LoRa SF7 equivalent: 118 ms x 29 mA ≈ **0.001 mAh/message** - roughly **80x**
  cheaper per message; the radio is not where a LoRa node's budget goes.

The store-and-forward queue changes this calculus in MQTT's favor: batching a
backlog through ONE connection amortizes the 2.5 s connect burst over the
whole queue - the marginal drained message costs ~0.15 s x 120 mA ≈
0.005 mAh, within 5x of LoRa. Outages make batching automatic.

## Why MQTT wins here anyway

- **Broker semantics are the product.** Fan-out to the dedup subscriber, a
  dashboard and an archiver without touching firmware; retained state;
  per-device topics with ACLs. CoAP and raw TCP both need a custom server
  that reimplements this.
- **Session resumption is protocol-native.** `clean_session=false` +
  `session present` (below) - CoAP/DTLS resumption and NAT rebinding are
  where UDP designs quietly rot **[assumed from ops experience, not measured
  here]**.
- **The engine already assumes at-least-once.** MQTT QoS1 maps exactly onto
  publish -> PUBACK -> erase. The transport is a plug-in (`saf_ops`); moving
  this fleet to CoAP or LTE-M raw sockets later changes one binding, not the
  design.

## QoS 1 + idempotent IDs, not QoS 2

QoS 2's four-way handshake (PUBLISH/PUBREC/PUBREL/PUBCOMP) doubles round
trips and broker session state to give *broker-boundary* exactly-once. It
still does not give **end-to-end** exactly-once: a consumer that crashes
after processing but before acking replays anyway, and broker persistence has
its own limits (the chaos suite's mosquitto SIGKILL drops QoS2 state exactly
as it drops QoS1 state). Any system that needs exactly-once *semantics* ends
up with idempotent consumers regardless - so pay for dedup once, at the
application layer where it also covers reboots and broker failover, and run
the wire at QoS1. The chaos suite proves the pair: the swallowed-PUBACK
scenario manufactures a real broker-side duplicate, the control (dedup off)
shows it reaching the consumer, the real pass shows it absorbed.

## What `clean_session=false` buys, precisely

With a persistent session the broker keeps, across our disconnects: the
client's subscriptions, and undelivered QoS1/2 messages *to* it. CONNACK's
`session present` flag says whether that state survived. For a pure publisher
the win is subtler but real: the broker remembers in-flight packet IDs, so a
reconnect-and-retry (DUP=1) is handled as the retransmission it is. The
engine deliberately does NOT trust any of it - its own NVS queue is the source
of truth (`session present=0` after the broker SIGKILL scenario is expected
and logged, and recovery works anyway, because the broker's memory is a cache
of ours, never the reverse).
