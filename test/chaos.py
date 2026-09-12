#!/usr/bin/env python3
"""chaos.py - the authoritative acceptance test.

Runs the exact saf_core.c the firmware ships (as build/host_device)
against a real mosquitto, with a cuttable TCP proxy in between, and
breaks everything the field breaks:

  1 healthy          - baseline delivery
  2 broker-sigkill   - mosquitto SIGKILLed mid-stream and restarted
  3 cut-mid-publish  - the proxy forwards HALF a PUBLISH frame, then RST
  4 swallowed-ack    - full PUBLISH forwarded, PUBACK never comes back:
                       the guaranteed broker-side duplicate. Run twice -
                       first with dedup disabled (CONTROL: the verdict
                       MUST be BROKEN), then for real (must be clean)
  5 reboot-mid-drain - device SIGKILLed with the queue half-drained
                       mid-outage; its only memory is the file store
  6 power-cut-torn   - the store tears a write mid-operation and kills
                       the process; recovery must detect it - and the
                       CONTROL (--no-crc-check) must NOT, delivering
                       garbage downstream

Zero loss and zero duplicates are asserted by the SUBSCRIBER's own
verdict line after every scenario; this script only orchestrates and
greps. Exit 0 means every scenario and both controls held.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RUN = os.path.join(HERE, "run")
DEVICE_BIN = os.path.join(HERE, "build", "host_device")


def find_mosquitto():
    # SAF_BROKER_CMD overrides for machines without mosquitto (a
    # stand-in must accept `-c <conf>`); CI always runs the real thing.
    override = os.environ.get("SAF_BROKER_CMD")
    if override:
        return override
    for cand in (shutil.which("mosquitto"), "/usr/local/sbin/mosquitto",
                 "/opt/homebrew/sbin/mosquitto", "/usr/sbin/mosquitto"):
        if cand and os.path.exists(cand):
            return cand
    sys.exit("chaos: mosquitto not found - install it "
             "(apt-get install mosquitto / brew install mosquitto)")


MOSQUITTO = find_mosquitto()


class Fail(Exception):
    pass


# --------------------------------------------------------------- the proxy --
def rst(sock):
    """Close with an RST, not a polite FIN - this is a cut, not a hangup."""
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        struct.pack("ii", 1, 0))
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


class Proxy:
    """TCP proxy between device and broker that can go down (connection
    refused), cut the k-th PUBLISH mid-frame ('mid'), or swallow its
    PUBACK ('ack'). Cut modes fire once, then traffic flows again."""

    def __init__(self, listen_port, broker_port):
        self.listen_port = listen_port
        self.broker_port = broker_port
        self.mode = None          # None | ("mid", k) | ("ack", k)
        self.publishes = 0
        self.cut_done = threading.Event()
        self._listener = None
        self._conns = []
        self._lock = threading.Lock()

    def up(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", self.listen_port))
        s.listen(4)
        self._listener = s
        threading.Thread(target=self._accept_loop, args=(s,),
                         daemon=True).start()

    def down(self):
        s, self._listener = self._listener, None
        if s:
            try:
                s.close()
            except OSError:
                pass
        with self._lock:
            conns, self._conns = self._conns, []
        for c in conns:
            rst(c)

    def _accept_loop(self, listener):
        while True:
            try:
                dev, _ = listener.accept()
            except OSError:
                return  # listener closed: proxy went down
            try:
                brk = socket.create_connection(
                    ("127.0.0.1", self.broker_port), timeout=1)
            except OSError:
                rst(dev)
                continue
            with self._lock:
                self._conns += [dev, brk]
            swallow = threading.Event()
            threading.Thread(target=self._pump_dev_to_broker,
                             args=(dev, brk, swallow), daemon=True).start()
            threading.Thread(target=self._pump_broker_to_dev,
                             args=(brk, dev, swallow), daemon=True).start()

    def _pump_broker_to_dev(self, brk, dev, swallow):
        while True:
            try:
                data = brk.recv(4096)
            except OSError:
                return
            if not data or swallow.is_set():
                return  # swallowed: the PUBACK dies here
            try:
                dev.sendall(data)
            except OSError:
                return

    def _pump_dev_to_broker(self, dev, brk, swallow):
        """Forward frame by frame so a cut lands exactly where we aim."""
        buf = b""
        while True:
            frame = None
            # try to peel one complete MQTT frame off the buffer
            if len(buf) >= 2:
                rem, mult, i = 0, 1, 1
                while i < len(buf):
                    d = buf[i]
                    i += 1
                    rem += (d & 0x7F) * mult
                    if not d & 0x80:
                        break
                    mult *= 128
                else:
                    i = None  # varint incomplete
                if i is not None and len(buf) >= i + rem:
                    frame, buf = buf[:i + rem], buf[i + rem:]

            if frame is None:
                try:
                    data = dev.recv(4096)
                except OSError:
                    return
                if not data:
                    return
                buf += data
                continue

            is_publish = frame[0] >> 4 == 3
            if is_publish:
                self.publishes += 1

            mode = self.mode
            if is_publish and mode and self.publishes == mode[1]:
                self.mode = None
                if mode[0] == "mid":
                    # half a PUBLISH, then scissors
                    try:
                        brk.sendall(frame[:len(frame) // 2])
                    except OSError:
                        pass
                    rst(dev)
                    rst(brk)
                    self.cut_done.set()
                    return
                # "ack": the broker gets the whole message, the device
                # never hears back about it
                swallow.set()
                try:
                    brk.sendall(frame)
                except OSError:
                    pass
                time.sleep(0.05)
                rst(dev)
                rst(brk)
                self.cut_done.set()
                return

            try:
                brk.sendall(frame)
            except OSError:
                return


# --------------------------------------------------------- child processes --
class Child:
    """Popen wrapper that tees stdout and lets scenarios wait on lines."""

    def __init__(self, name, argv, env=None, stdin=False):
        self.name = name
        self.lines = []
        self._cv = threading.Condition()
        self.proc = subprocess.Popen(
            argv, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            text=True, env=env)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            print(f"[{self.name}] {line}", flush=True)
            with self._cv:
                self.lines.append(line)
                self._cv.notify_all()

    def count(self, needle):
        with self._cv:
            return sum(needle in l for l in self.lines)

    def wait_line(self, needle, timeout, min_count=1):
        deadline = time.monotonic() + timeout
        with self._cv:
            while sum(needle in l for l in self.lines) < min_count:
                left = deadline - time.monotonic()
                if left <= 0:
                    raise Fail(f"{self.name}: timed out waiting for "
                               f"{needle!r} x{min_count}")
                self._cv.wait(left)

    def send(self, text):
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()

    def wait_exit(self, timeout):
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            raise Fail(f"{self.name}: did not exit") from None

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


# ------------------------------------------------------------ the scaffold --
class Rig:
    """One scenario's world: broker, proxy, subscriber, store dir."""

    N = 0

    def __init__(self, name):
        Rig.N += 1
        self.name = name
        self.dir = os.path.join(RUN, f"{Rig.N:02d}-{name}")
        os.makedirs(self.dir)
        self.store = os.path.join(self.dir, "store")
        os.makedirs(self.store)
        self.broker_port = 18800 + Rig.N * 10
        self.proxy_port = self.broker_port + 1
        self.expected_file = os.path.join(self.dir, "expected.txt")
        self.expected = []
        self.broker = None
        self.children = []

        conf = os.path.join(self.dir, "mosquitto.conf")
        with open(conf, "w") as f:
            # mosquitto 2.x denies everything by default; this is a
            # loopback test broker, open it up
            f.write(f"listener {self.broker_port} 127.0.0.1\n"
                    "allow_anonymous true\npersistence false\n")
        self.conf = conf

        self.proxy = Proxy(self.proxy_port, self.broker_port)

    def start_broker(self):
        self.broker = Child("broker", [*MOSQUITTO.split(), "-c", self.conf])
        self._wait_port(self.broker_port)

    def _wait_port(self, port, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                socket.create_connection(("127.0.0.1", port),
                                         timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise Fail(f"port {port} never opened")

    def start_subscriber(self, no_dedup=False):
        argv = [sys.executable, os.path.join(HERE, "subscriber.py"),
                "--port", str(self.broker_port),
                "--expected", self.expected_file]
        if no_dedup:
            argv.append("--no-dedup")
        self.sub = Child("sub", argv, stdin=True)
        self.children.append(self.sub)
        self.sub.wait_line("saf-sub: ready", 10)

    def start_device(self, count, interval_ms=50, extra=(), torn_at=None):
        env = dict(os.environ)
        if torn_at is not None:
            env["SAF_TORN_AT_PUT"] = str(torn_at)
        dev = Child("dev", [DEVICE_BIN,
                            "--store", self.store,
                            "--host", "127.0.0.1",
                            "--port", str(self.proxy_port),
                            "--device", "dev01",
                            "--count", str(count),
                            "--interval-ms", str(interval_ms),
                            *extra], env=env)
        self.children.append(dev)
        return dev

    def collect_expected(self, dev):
        """The device's saf-enq lines are the ground truth of what was
        durably accepted; the verdict is measured against their union
        across every device life in the scenario."""
        with dev._cv:
            for l in dev.lines:
                if l.startswith("saf-enq: "):
                    self.expected.append(int(l.split()[1]))

    def verdict(self, expect_pass=True, expect_in_line=None):
        with open(self.expected_file, "w") as f:
            f.write("\n".join(str(x) for x in self.expected) + "\n")
        self.sub.send("verdict")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            with self.sub._cv:
                for l in self.sub.lines:
                    if l.startswith("saf: ") or l.startswith(
                            "saf DELIVERY BROKEN"):
                        ok = l.startswith("saf: 0 lost")
                        if ok != expect_pass:
                            raise Fail(f"verdict {l!r}, wanted "
                                       f"pass={expect_pass}")
                        if expect_in_line and expect_in_line not in l:
                            raise Fail(f"verdict {l!r} lacks "
                                       f"{expect_in_line!r}")
                        return l
                self.sub._cv.wait(0.2)
        raise Fail("no verdict from subscriber")

    def teardown(self):
        self.proxy.down()
        for c in self.children:
            c.kill()
        if self.broker:
            self.broker.kill()


def run_scenario(fn):
    name = fn.__name__.replace("s_", "").replace("_", "-")
    print(f"\n=== chaos: {name} ===", flush=True)
    rig = Rig(name)
    try:
        rig.start_broker()
        fn(rig)
        print(f"=== chaos: {name} OK ===", flush=True)
    finally:
        rig.teardown()


# -------------------------------------------------------------- scenarios --
def s_healthy(rig):
    rig.start_subscriber()
    rig.proxy.up()
    dev = rig.start_device(count=20)
    dev.wait_line("saf-done", 30)
    rig.collect_expected(dev)
    rig.verdict()


def s_broker_sigkill(rig):
    rig.start_subscriber()
    rig.proxy.up()
    dev = rig.start_device(count=30, interval_ms=100, extra=("--run-ms", "90000"))
    rig.sub.wait_line("saf-sub: got", 15, min_count=8)

    # murder the broker mid-stream; keep the proxy down too so the
    # device cannot outrace the subscriber's resubscribe on restart
    rig.proxy.down()
    rig.broker.kill()
    time.sleep(1.0)
    ready_before = rig.sub.count("saf-sub: ready")
    rig.start_broker()
    rig.sub.wait_line("saf-sub: ready", 15, min_count=ready_before + 1)
    rig.proxy.up()

    dev.wait_line("saf-done", 60)
    if dev.count("saf-session") < 2:
        raise Fail("device never reconnected")
    rig.collect_expected(dev)
    rig.verdict()


def s_cut_mid_publish(rig):
    rig.start_subscriber()
    rig.proxy.mode = ("mid", 5)
    rig.proxy.up()
    dev = rig.start_device(count=12, interval_ms=100)
    dev.wait_line("saf-done", 45)
    if not rig.proxy.cut_done.is_set():
        raise Fail("the mid-publish cut never fired")
    if dev.count("saf-session") < 2:
        raise Fail("device never had to reconnect")
    rig.collect_expected(dev)
    rig.verdict()


def s_swallowed_ack_control(rig):
    # CONTROL: dedup disabled downstream. The swallowed PUBACK forces a
    # retransmit the broker takes as a second message; without dedup
    # the verdict MUST come out broken, or the dedup test means nothing.
    rig.start_subscriber(no_dedup=True)
    rig.proxy.mode = ("ack", 4)
    rig.proxy.up()
    dev = rig.start_device(count=10, interval_ms=100)
    dev.wait_line("saf-done", 45)
    if not rig.proxy.cut_done.is_set():
        raise Fail("the ack swallow never fired")
    rig.collect_expected(dev)
    rig.verdict(expect_pass=False, expect_in_line="dup=1")
    print("chaos: CONTROL confirmed - without dedup the duplicate "
          "reaches the consumer", flush=True)


def s_swallowed_ack_dedup(rig):
    # the same wound, with the dedup on: absorbed, verdict clean
    rig.start_subscriber()
    rig.proxy.mode = ("ack", 4)
    rig.proxy.up()
    dev = rig.start_device(count=10, interval_ms=100)
    dev.wait_line("saf-done", 45)
    if not rig.proxy.cut_done.is_set():
        raise Fail("the ack swallow never fired")
    rig.sub.wait_line("saf-sub: duplicate", 5)
    rig.collect_expected(dev)
    rig.verdict()


def s_reboot_mid_drain(rig):
    rig.start_subscriber()
    # outage first: the whole batch queues to the store
    dev = rig.start_device(count=10, interval_ms=50,
                           extra=("--run-ms", "90000"))
    dev.wait_line("saf-enq: ", 15, min_count=10)

    rig.proxy.up()  # link returns, drain starts...
    rig.sub.wait_line("saf-sub: got", 20, min_count=4)
    dev.kill()      # ...and the device dies mid-drain,
    rig.proxy.down()  # still mid-outage
    rig.collect_expected(dev)

    dev2 = rig.start_device(count=5, interval_ms=50,
                            extra=("--run-ms", "90000"))
    dev2.wait_line("saf-enq: ", 15, min_count=5)
    rig.proxy.up()
    dev2.wait_line("saf-done", 60)
    rig.collect_expected(dev2)
    rig.verdict()


def s_power_cut_torn_write(rig):
    rig.start_subscriber()
    # no link at all: this scenario is about the store
    dev = rig.start_device(count=10, torn_at=6,
                           extra=("--run-ms", "90000"))
    if dev.wait_exit(15) != 37:
        raise Fail("device did not die inside the torn write")
    rig.collect_expected(dev)  # 5 made it; the 6th tore

    dev2 = rig.start_device(count=3, extra=("--run-ms", "90000"))
    dev2.wait_line("saf-recovered: dropped 1 torn record", 10)
    rig.proxy.up()
    dev2.wait_line("saf-done", 60)
    rig.collect_expected(dev2)
    rig.verdict()


def s_power_cut_torn_control(rig):
    # CONTROL: same power cut, CRC check stubbed out. Detection MUST
    # fail and the half-written garbage MUST reach the consumer - if it
    # doesn't, the detector's green above proved nothing.
    rig.start_subscriber()
    dev = rig.start_device(count=10, torn_at=6,
                           extra=("--run-ms", "90000"))
    if dev.wait_exit(15) != 37:
        raise Fail("device did not die inside the torn write")
    rig.collect_expected(dev)

    dev2 = rig.start_device(count=0, extra=("--no-crc-check",
                                            "--run-ms", "90000"))
    dev2.wait_line("saf-boot", 10)
    if dev2.count("saf-recovered") != 0:
        raise Fail("crc-less device detected the torn record anyway - "
                   "the control is broken")
    rig.proxy.up()
    dev2.wait_line("saf-done", 60)
    rig.sub.wait_line("saf-sub: CORRUPT", 10)
    rig.verdict(expect_pass=False, expect_in_line="corrupt=1")
    print("chaos: CONTROL confirmed - without the CRC check the torn "
          "record is published as garbage", flush=True)


# ------------------------------------------------------------------- main --
def main():
    if os.path.exists(RUN):
        shutil.rmtree(RUN)
    os.makedirs(RUN)
    if not os.path.exists(DEVICE_BIN):
        sys.exit("chaos: build/host_device missing - run `make` first")

    for fn in (s_healthy, s_broker_sigkill, s_cut_mid_publish,
               s_swallowed_ack_control, s_swallowed_ack_dedup,
               s_reboot_mid_drain, s_power_cut_torn_write,
               s_power_cut_torn_control):
        try:
            run_scenario(fn)
        except Fail as e:
            print(f"\nchaos: SCENARIO FAILED - {e}", flush=True)
            sys.exit(1)

    print("\nchaos: ALL SCENARIOS PASS - zero loss, zero duplicates "
          "after dedup, both controls fired", flush=True)


if __name__ == "__main__":
    main()
