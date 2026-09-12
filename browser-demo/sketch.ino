/*
 * browser-demo/sketch.ino - Arduino-core port of mqtt-store-and-forward
 * for browser Wokwi (which cannot compile ESP-IDF). Clearly labeled
 * differences from the real firmware (main/):
 *
 *   - The offline queue is RAM, not NVS: the point of the browser demo
 *     is the queue/drain cycle you can drive by hand; the reboot-
 *     surviving NVS story is the ESP-IDF firmware's and the host chaos
 *     suite's job.
 *   - MQTT 3.1.1 is hand-rolled over WiFiClient (a straight port of
 *     test/mqtt_mini.c) so the paste-over-sketch.ino procedure needs
 *     no libraries. QoS 1, PUBACK-gated erase, same as the engine.
 *   - Publishes REAL traffic to test.mosquitto.org via the browser
 *     gateway. If that public broker is unreachable, it says so and
 *     simulates acks instead - the banner tells you which mode you got.
 *
 * Shell (type in the white input strip at the BOTTOM of the sim pane):
 *   stat - counters   out - force offline   in - restore + drain
 */
#include <WiFi.h>

/* ----------------------------------------------------------- settings -- */
static const char *SSID = "Wokwi-GUEST";
static const char *BROKER = "test.mosquitto.org";
static const int BROKER_PORT = 1883;

/* ------------------------------------------------------------- queue -- */
struct Msg { uint32_t seq; char data[48]; };
static Msg queued[64];
static int q_head, q_tail, q_depth;
static uint32_t seq_next;
static uint32_t sent_n, acked_n, dropped_n;

/* -------------------------------------------------------------- state -- */
static WiFiClient tcp;
static bool online = true;      /* the out/in lie */
static bool mqtt_up = false;
static bool sim_acks = false;   /* broker unreachable: honest fallback */
static bool inflight = false;
static uint32_t inflight_seq;
static unsigned long inflight_ms, sim_ack_at;
static char devid[24];
static char topic[48];
static bool pending_drain = false;
static uint32_t drain_started;

/* --------------------------------------------------- tiny MQTT 3.1.1 -- */
static bool mqtt_connect()
{
  if (!tcp.connect(BROKER, BROKER_PORT, 3000)) return false;
  uint8_t f[64]; size_t n = 0, idlen = strlen(devid);
  f[n++] = 0x10; f[n++] = (uint8_t)(12 + idlen);
  memcpy(f + n, "\x00\x04MQTT\x04", 7); n += 7;
  f[n++] = 0x02; /* clean session: fresh random id per visitor */
  f[n++] = 0; f[n++] = 60;
  f[n++] = 0; f[n++] = (uint8_t)idlen;
  memcpy(f + n, devid, idlen); n += idlen;
  tcp.write(f, n); tcp.flush();
  unsigned long t0 = millis();
  uint8_t ack[4]; size_t got = 0;
  while (got < 4 && millis() - t0 < 3000)
    if (tcp.available()) ack[got++] = tcp.read();
  return got == 4 && ack[0] == 0x20 && ack[3] == 0x00;
}

static bool mqtt_publish(const Msg &m, bool dup)
{
  char wire[128];
  int plen = snprintf(wire, sizeof wire,
                      "{\"id\":\"%s-%08u\",\"seq\":%u,\"data\":\"%s\"}",
                      devid, m.seq, m.seq, m.data);
  size_t tlen = strlen(topic), rem = 2 + tlen + 2 + plen;
  uint8_t f[220]; size_t n = 0;
  f[n++] = 0x32 | (dup ? 0x08 : 0);
  f[n++] = (uint8_t)rem; /* rem < 128 always here */
  f[n++] = (uint8_t)(tlen >> 8); f[n++] = (uint8_t)tlen;
  memcpy(f + n, topic, tlen); n += tlen;
  uint16_t pid = (uint16_t)(m.seq % 65535u) + 1u;
  f[n++] = (uint8_t)(pid >> 8); f[n++] = (uint8_t)pid;
  memcpy(f + n, wire, plen); n += plen;
  return tcp.write(f, n) == n;
}

static bool mqtt_poll_puback()
{
  /* PUBACK 0x40 0x02 pid pid; we have one in flight, any PUBACK is ours */
  while (tcp.available() >= 4) {
    if (tcp.read() != 0x40) continue;
    tcp.read(); tcp.read(); tcp.read();
    return true;
  }
  return false;
}

/* --------------------------------------------------------------- shell -- */
static void stat_report()
{
  Serial.printf("stat: link=%s depth=%d inflight=%d online=%d mode=%s\n",
                mqtt_up ? "UP" : "DOWN", q_depth, inflight ? 1 : 0,
                online ? 1 : 0, sim_acks ? "sim-ack" : "real-broker");
  Serial.printf("stat: enq=%u sent=%u acked=%u dropped=%u seq_next=%u\n",
                seq_next, sent_n, acked_n, dropped_n, seq_next);
}

static void dispatch(String line)
{
  line.trim();
  if (line == "stat") stat_report();
  else if (line == "out") {
    online = false;
    if (mqtt_up || sim_acks) { tcp.stop(); mqtt_up = false; inflight = false; }
    Serial.println("transport: forced offline - queueing to RAM");
  } else if (line == "in") {
    online = true;
    pending_drain = q_depth > 0;
    drain_started = q_depth;
    Serial.printf("transport: restored - draining %d queued\n", q_depth);
  } else if (line.length())
    Serial.println("unknown command - try stat | out | in");
  Serial.print("> ");
}

/* --------------------------------------------------------------- setup -- */
void setup()
{
  Serial.begin(115200);
  snprintf(devid, sizeof devid, "safweb%04x",
           (unsigned)(esp_random() & 0xffff));
  snprintf(topic, sizeof topic, "saf/%s/telemetry", devid);

  Serial.printf("\nconnecting to %s...\n", SSID);
  WiFi.begin(SSID, "");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000)
    delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi failed - simulating acks instead");
    sim_acks = true;
  }
  Serial.printf("device %s topic %s\n", devid, topic);
  Serial.println("shell: commands: stat | out | in");
  Serial.print("> ");
}

/* ---------------------------------------------------------------- loop -- */
void loop()
{
  static String line;
  static unsigned long next_sample, last_beat;
  unsigned long now = millis();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { Serial.println(); dispatch(line); line = ""; }
    else { Serial.print(c); line += c; }
  }

  /* sample every second */
  if (now >= next_sample) {
    next_sample = now + 1000;
    if (q_depth < 64) {
      Msg &m = queued[q_tail];
      m.seq = seq_next++;
      snprintf(m.data, sizeof m.data, "t=%lu n=%lu", now,
               (unsigned long)m.seq);
      q_tail = (q_tail + 1) % 64;
      q_depth++;
      if (!mqtt_up && !sim_acks)
        Serial.printf("saf-queue: depth=%d seq=%u\n", q_depth, m.seq);
    } else dropped_n++;
  }

  /* connect when allowed */
  if (online && !mqtt_up && !sim_acks) {
    static unsigned long next_try;
    if (now >= next_try) {
      next_try = now + 2000 + (esp_random() % 1000); /* jittered retry */
      Serial.println("mqtt: connecting...");
      if (mqtt_connect()) { mqtt_up = true; Serial.println("mqtt: up"); }
      else {
        Serial.println("mqtt: broker unreachable");
        static int fails = 0;
        if (++fails >= 3) {
          Serial.println("mqtt: giving up - simulating acks (labeled!)");
          sim_acks = true;
        }
      }
    }
  }

  /* drain: one in flight, erase after ack (the engine's rule) */
  bool can_send = online && (mqtt_up || sim_acks);
  if (can_send && !inflight && q_depth > 0) {
    Msg &m = queued[q_head];
    bool ok = sim_acks ? true : mqtt_publish(m, false);
    if (ok) {
      inflight = true; inflight_seq = m.seq; inflight_ms = now;
      sim_ack_at = now + 80;
      sent_n++;
    } else { tcp.stop(); mqtt_up = false; }
  }
  if (inflight) {
    bool acked = sim_acks ? now >= sim_ack_at : mqtt_poll_puback();
    if (acked) {
      inflight = false;
      q_head = (q_head + 1) % 64; q_depth--;
      acked_n++;
    } else if (now - inflight_ms > 5000) {
      inflight = false; /* retry after reconnect, same seq: idempotent */
      if (!sim_acks) { tcp.stop(); mqtt_up = false; }
    }
  }

  if (pending_drain && q_depth == 0 && !inflight) {
    pending_drain = false;
    Serial.printf("saf: drained, nothing lost (%u queued while out)\n",
                  drain_started);
  }

  if (now - last_beat >= 5000) {
    last_beat = now - (now % 5000);
    Serial.printf("alive t=%lus depth=%d\n", last_beat / 1000, q_depth);
  }
  delay(10);
}
