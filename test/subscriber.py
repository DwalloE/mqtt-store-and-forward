#!/usr/bin/env python3
"""subscriber.py - the downstream half of the acceptance test.

Subscribes (QoS 1, via paho - deliberately an independent client, not
the code under test) and keys every message on its idempotent id.
When chaos.py writes the expected-sequence file (built from the
device's own `saf-enq:` lines) and sends "verdict" on stdin, this
process - and only this process - computes the pass/fail:

    saf: 0 lost, 0 duplicated after dedup          <- CI greps this
    saf DELIVERY BROKEN: missing=.. dup=.. unexpected=.. corrupt=..

The failure text shares no substring with the pass text (01's
verdict-line rule). --no-dedup is the CONTROL mode: raw deliveries are
scored without dedup, so a chaos pass that swallowed a PUBACK MUST
come out BROKEN here - proving the dedup, not luck, is what makes the
real pass clean.
"""
import argparse
import json
import sys

import paho.mqtt.client as mqtt


def log(line):
    print(line, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--topic", default="saf/+/telemetry")
    ap.add_argument("--expected", required=True,
                    help="file of expected sequence numbers, one per line")
    ap.add_argument("--no-dedup", action="store_true",
                    help="CONTROL: score raw deliveries, duplicates count")
    args = ap.parse_args()

    raw = []          # every delivery, in arrival order
    seen = set()      # after dedup on the message id
    absorbed = 0      # raw duplicates the dedup swallowed
    corrupt = 0       # unparseable / self-inconsistent payloads

    def on_message(_cli, _ud, msg):
        nonlocal absorbed, corrupt
        try:
            body = json.loads(msg.payload.decode("utf-8"))
            dev, seq = body["id"].rsplit("-", 1)
            seq = int(seq)
            # the payload must agree with itself: id vs seq field
            if body["seq"] != seq or not dev:
                raise ValueError("id/seq mismatch")
        except (ValueError, KeyError, UnicodeDecodeError):
            corrupt += 1
            log(f"saf-sub: CORRUPT payload ({msg.payload[:40]!r})")
            return
        raw.append(seq)
        if seq in seen:
            absorbed += 1
            log(f"saf-sub: duplicate seq={seq} (raw)")
        else:
            seen.add(seq)
            log(f"saf-sub: got seq={seq}")

    def on_connect(cli, _ud, _flags, _rc, *_props):
        cli.subscribe(args.topic, qos=1)

    def on_subscribe(*_a):
        log("saf-sub: ready")

    try:  # paho 2.x
        cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:  # paho 1.x
        cli = mqtt.Client()
    cli.on_connect = on_connect
    cli.on_subscribe = on_subscribe
    cli.on_message = on_message
    cli.reconnect_delay_set(min_delay=1, max_delay=1)
    cli.connect("127.0.0.1", args.port)
    cli.loop_start()

    # chaos.py drives us over stdin
    for line in sys.stdin:
        cmd = line.strip()
        if cmd == "verdict":
            with open(args.expected) as f:
                expected = {int(x) for x in f.read().split()}
            if args.no_dedup:
                got = raw
                dup = len(raw) - len(set(raw))
            else:
                got = seen
                dup = 0  # by construction; `absorbed` says what it ate
            missing = expected - set(got)
            unexpected = set(got) - expected
            log(f"saf-sub: absorbed={absorbed} raw={len(raw)} "
                f"expected={len(expected)}")
            if missing or dup or unexpected or corrupt:
                log(f"saf DELIVERY BROKEN: missing={len(missing)} "
                    f"dup={dup} unexpected={len(unexpected)} "
                    f"corrupt={corrupt}")
            else:
                log("saf: 0 lost, 0 duplicated after dedup")
        elif cmd == "quit":
            break
    cli.loop_stop()


if __name__ == "__main__":
    main()
