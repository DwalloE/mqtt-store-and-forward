# The NVS-backed offline queue: wear, torn writes, and the duplicate window

Everything here is about `main/nvs_store.c` (one blob per advancing key) and the
recovery path in `main/saf_core.c` (`saf_init`'s tail trim). The numbers are
datasheet arithmetic, clearly labeled - this repo has no bench and no endurance
rig, and the README's honest-limits section says so.

## How NVS actually wears

NVS is a log-structured key-value store. A partition is a ring of 4096-byte
flash sectors ("pages"); every write **appends** a new 32-byte entry (blobs
span several entries) and marks the previous version of that key erased. A
sector is only physically erased when the log wraps and its live entries have
been compacted elsewhere. Two consequences matter here:

1. **Updating the same key over and over does not hammer one sector.** NVS's
   log structure is itself a wear-leveler.
2. **What you pay per write is entries appended**, and sector erases amortize
   over everything appended since the last wrap.

## Why the queue advances keys instead of rewriting one

If wear-leveling is free, why not keep the whole queue as one `queue` blob and
rewrite it on every enqueue/ack? Two reasons, and only the second is wear:

- **Crash blast radius.** A torn write of a whole-queue blob risks the whole
  queue; NVS would fall back to the previous version of the blob - which
  silently *resurrects* already-acked messages and *forgets* accepted ones.
  With one record per key, the only thing a power cut can damage is the single
  record being appended, and `saf_init` trims exactly that.
- **Write amplification.** A whole-queue blob is `depth x record` bytes per
  operation. At depth 64 and ~110-byte records that is ~7 KB - nearly two full
  pages - per enqueue *and* per ack. One record per key appends ~7 entries
  (~224 bytes) per enqueue and 1 erase-marker entry per ack.

### The erase-cycle arithmetic (datasheet math, not a measurement)

Assumptions, all stated: 100k program/erase cycles per sector (typical for the
GD25Q32-class NOR flash on ESP32 modules), the default 24 KB (6-sector) `nvs`
partition, ~110-byte records (blob = ~6 entries) plus an erase marker and the
1-in-32 sequence-ceiling update, so ~7.1 entries ≈ 227 bytes appended per
message end to end.

- One 4096-byte page holds 126 entries -> a message consumes ~7.1/126 ≈ 5.6% of
  a page -> **~0.056 sector erases per message** once the log is wrapping.
- Lifetime sector erases: 6 sectors x 100k = 600k -> **~10.7M messages** before
  the partition is worn out.
- At one message every 10 seconds (8,640/day): **~3.4 years** on the default
  24 KB partition; a 64 KB partition scales that to ~9 years.

The whole-queue-blob alternative under the same assumptions: ~2 pages appended
per message -> ~1 sector erase per message -> 600k messages -> **69 days** at
the same rate. The advancing-key layout is worth roughly **18x-36x** in flash
life depending on queue depth, before even counting the crash-safety argument.

(When the queue fully drains and the device reboots, key indices restart at 0.
That is deliberate and costs nothing: NVS appends a *new* entry for a reused
key like any other write.)

## The power-cut duplicate - by design

The delivery loop is publish -> PUBACK -> erase. Cut power between "the broker
acked it" and "the record left NVS" and the next boot finds the record still
queued, so it publishes it again. **That duplicate is the design working**:
the alternative orderings are worse (erase-before-ack turns the same cut into
data loss). It is exactly why every message carries an idempotent
`<device>-<seq>` identity - the downstream dedup keys on it, and the chaos
suite's swallowed-PUBACK scenario proves the pair (duplicate produced,
duplicate absorbed) end to end. The same argument covers a *failed* erase:
`saf_on_puback` counts it (`erase_failures`) and moves on, because the record
can only replay, never vanish.

## The torn write, and how we know the detector works

A power cut mid-append leaves a record whose header hit the flash and whose
tail never did - reading back full-length but with garbage (erased-cell 0xFF)
where payload and CRC should be. Structural checks (magic, length bounds)
catch short and mangled records; only the trailing CRC32 catches the
full-length-garbage-tail case.

The host chaos suite reproduces the cut literally: the file store's fault
writes half the record, fills the rest with 0xFF, and `_exit()`s **inside**
`store_put` - the process dies mid-operation like the silicon would. Recovery
must then report `saf-recovered: dropped 1 torn record` and deliver everything
that was durably enqueued.

**The control (02/03's doctrine: prove the net fires).** The same scenario
runs once more with the CRC check stubbed out (`--no-crc-check`). Detection
must *fail*, the garbage record must be published, and the subscriber must
score it corrupt - otherwise the green detector above proved nothing. The
unit suite pins the same pair deterministically
(`test_torn_tail_detected_and_dropped` / `test_torn_tail_control_without_crc_check`).

## Sequence numbers across reboots: the ceiling lease

Monotonic sequences must survive reboot without writing a counter to NVS per
message (that would double the write cost computed above). The engine persists
a **ceiling** every `SAF_SEQ_LEASE` (32) messages, *before* issuing the first
sequence past the previous ceiling; on boot it resumes from
`max(ceiling, newest surviving record + 1)`. A reboot may therefore *skip* up
to 31 numbers but can never reuse one - which is the only property dedup
needs. The chaos verdict is measured against the device's own `saf-enq:`
lines, not against sequence density, so leased gaps are not "loss".
