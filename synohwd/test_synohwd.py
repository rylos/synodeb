#!/usr/bin/python3 -I
"""Offline tests for synohwd (no hardware needed): python3 -I synohwd/test_synohwd.py (or pytest)"""
import importlib.util
import os
import stat
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("synohwd", os.path.join(HERE, "synohwd.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

failures = 0


def check(name, got, want):
    global failures
    ok = got == want
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}" + ("" if ok else f" (expected {want!r})"))


class Feed:
    """Hand-driven parser with a fake clock."""

    def __init__(self):
        self.events, self.replies, self.now = [], [], 1000.0
        self.p = m.Parser(self.events.append, self.replies.append)

    def want(self, cmd):
        self.p.expect, self.p.expect_until = cmd, self.now + 1.0

    def bytes(self, data, step=0.0001):
        for b in data:
            self.now += step
            self.p.feed(b, self.now)
        return self

    def wait(self, s):
        self.now += s
        self.p.tick(self.now)
        return self


# --- parser ---
f = Feed(); f.want("D"); f.bytes("D123X")
check("D normal", (f.replies, f.events), (["123"], []))
f = Feed(); f.want("D"); f.bytes("D*2X")
check("D with '*'", f.replies, ["02"])
f = Feed(); f.want("D"); f.bytes("D12a")
check("D without X, then reset 'a'", (f.replies, f.events), ([], ["a"]))
f = Feed(); f.want("D"); f.bytes("D12").wait(0.6).bytes("0")
check("D timed out (0.5 s), then power '0'", (f.replies, f.events), ([], ["0"]))
f = Feed(); f.want("D"); f.bytes("D12"); f.now += 0.6; f.bytes("0")
check("D timed out without tick, power '0'", f.events, ["0"])
f = Feed(); f.bytes("D0")
check("unsolicited 'D': power not lost", f.events, ["D", "0"])
f = Feed(); f.want("D"); f.bytes("D123456789X0")
check("D too long", (f.replies, f.events), ([], ["X", "0"]))
f = Feed(); f.want("R"); f.bytes("+3abc0")
check("valid '+' frame", (f.replies, f.events), (["abc"], ["0"]))
f = Feed(); f.want("R"); f.bytes("+\xff0")
check("frame with absurd length", (f.replies, f.events), ([], ["\xff", "0"]))
f = Feed(); f.want("R"); f.bytes("+0")
check("frame length 0 = power", f.events, ["0"])
f = Feed(); f.want("R"); f.bytes("+a")
check("frame length 'a' = reset", f.events, ["a"])
f = Feed(); f.want("R"); f.bytes("+9ab").wait(0.6).bytes("a")
check("truncated frame, then reset", (f.replies, f.events), ([], ["a"]))
f = Feed(); f.bytes("+3abc")
check("unsolicited '+'", f.events, list("+3abc"))

# real reader on a pipe, MicroP without UART
r, w = os.pipe()
up = m.MicroP.__new__(m.MicroP); up.fd = r; up.init_common()
ev, err = [], []


def reader():
    try:
        up.reader(ev.append)
    except OSError as e:
        err.append(str(e))


t = threading.Thread(target=reader, daemon=True); t.start()
os.write(w, b"0a")
time.sleep(0.3)
check("reader: events", ev, ["0", "a"])
up.parser.want("D", 1.0); os.write(w, b"D42X"); time.sleep(0.3)
check("reader: D reply", up.reply, "42")
os.close(w); t.join(3)
check("reader: EOF -> exception", (t.is_alive(), err), (False, ["uP UART: end of file"]))

# --- '1' blocking ---
for cmd, ok in [("V10", True), ("V99", True), ("V01", True), ("V10V11", True), ("2", True),
                ("1", False), ("21", False), ("V1", False), ("V101", False), ("LA1", False),
                ("1V10", False), ("", False)]:
    try:
        m.check_cmd(cmd); got = True
    except ValueError:
        got = False
    check(f"check_cmd {cmd!r}", got, ok)

up2 = m.MicroP.__new__(m.MicroP); up2.init_common(); up2.fd = None
try:
    up2.send("21"); refused = False
except ValueError:
    refused = True
check("MicroP.send('21') refused before writing", refused, True)

# --- fan hysteresis (quiet disk table) ---
fan = m.Fan(dict(m.DEFAULTS)); tab = m.FAN_TABLES["low"]["disk"]
seq = [40, 47, 53, 50, 49, 45, 43, 42, 59, 56, 55, 54]
check("hysteresis", [fan.step("disk", x, tab) for x in seq],
      [50, 60, 70, 70, 60, 60, 50, 50, 99, 99, 80, 80])
fan = m.Fan(dict(m.DEFAULTS))
s = {"cpu": 50, "disk": {"bay1": 62}, "nvme": {}, "nic": {}}
duty, crit = fan.duty(s)
check("duty with a disk at 62 °C", (duty, crit is not None), (99, True))

# --- thermal shutdown: 2 consecutive readings ---
th = m.Thermal()
check("thermal: threshold, normal, threshold", [th.update(x) for x in ("c", None, "c")],
      [False, False, False])
check("thermal: second in a row", th.update("c"), True)

# --- absurd temperatures discarded ---
with tempfile.TemporaryDirectory() as d:
    for n, v in ((1, 200000), (2, 45000), (3, -30000)):
        with open(f"{d}/temp{n}_input", "w") as fh:
            fh.write(str(v))
    check("hwmon_temp discards >150 and <-20", m.hwmon_temp(d), 45.0)

# --- fan failure ---
fh_ = m.FanHealth()
check("alarm 1 cycle", fh_.update([1, 0], 0), False)
check("alarm 2 cycles", fh_.update([1, 0], 20), True)
check("alarm cleared", fh_.update([0, 0], 40), False)
fh_.event(100)
check("recent 'f' event", fh_.update([0, 0], 110), True)
check("'f' event after 60 s", fh_.update([0, None], 161), False)

# --- hdparm -C (fake) ---
with tempfile.TemporaryDirectory() as d:
    hp = f"{d}/hdparm"
    with open(hp, "w") as fh:
        fh.write('#!/bin/sh\nprintf "\\n%s:\\n drive state is:  $STATE\\n" "$2"\n')
    os.chmod(hp, stat.S_IRWXU)
    old = os.environ["PATH"]; os.environ["PATH"] = f"{d}:{old}"
    os.environ["STATE"] = "standby"; check("hdparm standby", m.disk_asleep("sda"), True)
    os.environ["STATE"] = "active/idle"; check("hdparm active", m.disk_asleep("sda"), False)
    os.environ["PATH"] = "/nonexistent"; check("hdparm missing", m.disk_asleep("sda"), None)
    os.environ["PATH"] = old

# --- disk LEDs: bay -> ATA port -> kernel trigger ---
check("led empty bay", m.led_target(False, None, None, None), ("none", 0, 0))
check("led disk not detected", m.led_target(True, None, None, None), ("none", 1, 0))
check("led disk ok", m.led_target(True, "sda", 5, "running"), ("ata5", 1, 0))
check("led disk in error", m.led_target(True, "sda", 5, "offline"), ("none", 0, 1))
with tempfile.TemporaryDirectory() as d:
    # fake sysfs: JMB585 (00:01.5) ports ata1..ata5, FCH (00:08.2) ata7, ata8
    jmb = f"{d}/devices/pci0000:00/0000:00:01.5/0000:02:00.0"
    fch = f"{d}/devices/pci0000:00/0000:00:08.2/0000:0c:00.0"
    for c, ports in ((jmb, (1, 2, 3, 4, 5)), (fch, (7, 8))):
        for n in ports:
            os.makedirs(f"{c}/ata{n}/host{n}/target{n}:0:0/{n}:0:0:0")
        os.makedirs(f"{c}/ata_link_x", exist_ok=True)      # non-numeric ata* name
    os.makedirs(f"{d}/block")
    for dev, c, n in (("sda", jmb, 2), ("sdb", fch, 7), ("sdc", fch, 8)):
        os.makedirs(f"{d}/block/{dev}")
        os.symlink(f"{c}/ata{n}/host{n}/target{n}:0:0/{n}:0:0:0", f"{d}/block/{dev}/device")
    with open(f"{fch}/ata8/host8/target8:0:0/8:0:0:0/state", "w") as fh:
        fh.write("offline\n")
    with open(f"{fch}/ata7/host7/target7:0:0/7:0:0:0/state", "w") as fh:
        fh.write("running\n")
    m.SYS_BLOCK = f"{d}/block"
    check("bay_disks", m.bay_disks(), {2: ("sda", 2), 3: ("sdb", 7), 4: ("sdc", 8)})
    check("bay_block_devices", m.bay_block_devices(), {2: "sda", 3: "sdb", 4: "sdc"})
    # fake LEDs and presence
    m.LEDS, m.PLATFORM = f"{d}/leds", f"{d}/platform"
    os.makedirs(m.PLATFORM)
    for b, p in ((1, 0), (2, 1), (3, 1), (4, 1)):
        with open(f"{m.PLATFORM}/bay{b}_present", "w") as fh:
            fh.write(f"{p}\n")
    for b in range(1, 5):
        for col in ("green", "orange"):
            os.makedirs(f"{m.LEDS}/syno:{col}:disk{b}")
            for a, v in (("trigger", "[none] disk-activity ata1 ata2 ata3 ata4 ata5 ata7 ata8"),
                         ("brightness", "9")):
                with open(f"{m.LEDS}/syno:{col}:disk{b}/{a}", "w") as fh:
                    fh.write(v)
    dm = m.Daemon.__new__(m.Daemon)

    def leds():
        return {b: tuple(m.read(f"{m.LEDS}/syno:{c}:disk{b}/{a}")
                         for c, a in (("green", "trigger"), ("green", "brightness"),
                                      ("orange", "brightness")))
                for b in range(1, 5)}

    shown = {}
    dm.led_step(shown, 255)
    check("led_step full brightness", leds(),
          {1: ("none", "0", "0"), 2: ("ata2", "9", "0"), 3: ("ata7", "9", "0"),
           4: ("none", "0", "255")})
    os.unlink(f"{d}/block/sdb/device")                        # disk 3 gone, bay occupied
    dm.led_step(shown, 255)
    check("led_step disk not detected", leds()[3], ("none", "255", "0"))
    shown = {}
    dm.led_step(shown, 40)
    check("led_step dimmed: trigger + brightness", leds()[2], ("ata2", "40", "0"))
    with open(f"{m.LEDS}/syno:green:disk2/trigger", "w") as fh:
        fh.write("[none] disk-activity")                      # kernel without ATA_LEDS
    shown = {}
    dm.led_step(shown, 255)
    check("led_step without ataN trigger: steady green", leds()[2], ("none", "255", "0"))

# --- synohwd send without the daemon ---
with tempfile.TemporaryDirectory() as d:
    m.CMD_FIFO = f"{d}/cmd"

    def send(cmd):
        try:
            m.send(cmd); return "ok"
        except SystemExit as e:
            return str(e)

    check("send without FIFO", "is not running" in send("2"), True)
    os.mkfifo(m.CMD_FIFO, 0o600)
    check("send FIFO without reader (ENXIO)", "nobody reads" in send("2"), True)
    rd = os.open(m.CMD_FIFO, os.O_RDWR)
    check("send with reader", send("V50"), "ok")
    check("send read back", os.read(rd, 64), b"V50\n")
    check("send '1' refused", send("1").startswith("refused"), True)

    try:
        real_write, m.os.write = m.os.write, lambda *_: (_ for _ in ()).throw(BlockingIOError())
        check("send FIFO full", "busy" in send("2"), True)
    finally:
        m.os.write = real_write
    os.close(rd)

# --- malformed uP commands ---
for cmd in ("VV11", "V", "V9", "VX1", "C", "2C", "LA1", "x", "V10 ", "V1a", "VVV"):
    try:
        m.check_cmd(cmd); got = True
    except ValueError:
        got = False
    check(f"check_cmd refuses {cmd!r}", got, False)
for cmd in ("D", "R", "t", "u", "@", "AB", ":;", "V0099"):
    try:
        m.check_cmd(cmd); got = True
    except ValueError:
        got = False
    check(f"check_cmd accepts {cmd!r}", got, True)

# --- duty fail-safe ---
OK = {"cpu": 40.0, "disk": {"bay1": 30.0}, "nvme": {"nvme0": 35.0}, "nic": {}}


def duty_of(s, fan_fail=False, **conf):
    return m.Fan(m.validate_conf(conf)).duty(s, fan_fail)


check("duty all cool", duty_of(OK), (50, None))
check("duty no sensors at all", duty_of({"cpu": None, "disk": {}, "nvme": {}, "nic": {}})[0], 99)
check("duty CPU missing", duty_of({**OK, "cpu": None})[0], 99)
check("duty disk without reading", duty_of({**OK, "disk": {"bay1": 30.0, "bay2": None}})[0], 99)
check("duty NVMe without reading", duty_of({**OK, "nvme": {"nvme0": None}})[0], 99)
check("duty NIC without reading is fine", duty_of({**OK, "nic": {"eth": None}})[0], 50)
check("duty fan failure", duty_of(OK, fan_fail=True), (99, None))
check("duty full mode keeps the critical reason",
      duty_of({**OK, "cpu": 96.0}, fan_mode="full"), (99, "cpu 96°C >= 95°C"))
check("duty 0.0 °C is a reading", duty_of({**OK, "cpu": 0.0, "disk": {"bay1": 0.0}}), (50, None))
fan = m.Fan(dict(m.DEFAULTS))
check("duty recovers after the sensor returns",
      [fan.duty(x)[0] for x in ({**OK, "cpu": None}, OK)], [99, 50])

# --- 0.0 °C readings in sensors() (fake hwmon / sysfs) ---
with tempfile.TemporaryDirectory() as d:
    real_by_name, real_path = m.hwmon_by_name, os.environ["PATH"]
    hw = {}
    for name, files in (("k10temp", {"temp1_input": "0", "temp1_label": "Tctl",
                                     "temp2_input": "50000"}),
                        ("nvme", {"temp1_input": "0", "temp1_label": "Composite",
                                  "temp2_input": "60000"})):
        os.makedirs(f"{d}/{name}")
        for fn, v in files.items():
            with open(f"{d}/{name}/{fn}", "w") as fh:
                fh.write(v)
        hw[name] = [f"{d}/{name}"]
    os.makedirs(f"{d}/ctrl/nvme0"); os.makedirs(f"{d}/ctrl/nvme1")
    os.makedirs(f"{d}/block/sda/device/hwmon/hwmon9"); os.makedirs(f"{d}/block/sdb/device")
    with open(f"{d}/block/sda/device/hwmon/hwmon9/temp1_input", "w") as fh:
        fh.write("0")
    m.hwmon_by_name = lambda n: hw.get(n, [])
    m.SYS_BLOCK, m.NVME_CLASS = f"{d}/block", f"{d}/ctrl"
    os.environ["PATH"] = "/nonexistent"                       # no hdparm
    try:
        s = m.sensors({1: "sda", 2: "sdb"})
    finally:
        m.hwmon_by_name, os.environ["PATH"] = real_by_name, real_path
    check("sensors: Tctl 0.0 °C kept", s["cpu"], 0.0)
    check("sensors: disk 0.0 °C kept, disk without drivetemp = None", s["disk"],
          {"bay1": 0.0, "bay2": None})
    check("sensors: NVMe Composite 0.0 °C kept, controller without hwmon = None",
          sorted(s["nvme"].values(), key=str), [0.0, None])

# --- fans: unreadable tach with the platform hwmon present ---
with tempfile.TemporaryDirectory() as d:
    for fn, v in (("fan1_input", "1200"), ("fan1_alarm", "0")):     # fan2 attributes missing
        with open(f"{d}/{fn}", "w") as fh:
            fh.write(v)
    fans = m.fans_of([d])
    check("fans_of lists both fans", fans,
          [{"fan": 1, "rpm": 1200, "alarm": 0}, {"fan": 2, "rpm": None, "alarm": None}])
    fh_ = m.FanHealth()
    check("unreadable tach: 1 cycle tolerated (ENODATA after probe)",
          fh_.update([f["alarm"] for f in fans], 0), False)
    check("unreadable tach: 2 cycles = failure", fh_.update([f["alarm"] for f in fans], 20), True)
    check("no platform hwmon: no fans", m.fans_of([]), [])

# --- config validation ---
check("conf defaults", m.validate_conf({}), m.DEFAULTS)
bad = {"power_button": True, "fan_period": 0, "fan_min_duty": 12.5, "hysteresis": -1,
       "fan_mode": "turbo", "led_brightness": 1000, "disk_standby_minutes": "20",
       "reset_button": "command:", "wol": 1, "nonsense": 3, "led_activity_hz": 4}
c = m.validate_conf(bad)
check("conf bool for a string -> default", c["power_button"], "poweroff")
check("conf fan_period 0 -> clamped to 5", c["fan_period"], 5)
check("conf float duty -> default", c["fan_min_duty"], 0)
check("conf hysteresis -1 -> 0", c["hysteresis"], 0)
check("conf unknown fan_mode -> default", c["fan_mode"], "low")
check("conf led_brightness 1000 -> 255", c["led_brightness"], 255)
check("conf string minutes -> default", c["disk_standby_minutes"], 0)
check("conf empty command -> default", c["reset_button"], "log")
check("conf int for a bool -> default", c["wol"], False)
check("conf unknown keys dropped", ("nonsense" in c, "led_activity_hz" in c), (False, False))
good = {"power_button": "command:/usr/local/bin/x", "fan_period": 60, "fan_min_duty": 70,
        "fan_mode": "high", "wol": True, "reset_button": "ignore"}
check("conf valid values kept", {k: m.validate_conf(good)[k] for k in good}, good)
check("conf only DEFAULTS keys", set(c), set(m.DEFAULTS))

# --- uP writes: EIO retried, then raised; fail-safe never raises ---
r, w = os.pipe()
up3 = m.MicroP.__new__(m.MicroP); up3.init_common(); up3.fd = w
calls = []


def flaky(fd, data, real=os.write):
    calls.append(data)
    if len(calls) == 1:
        raise OSError(m.errno.EIO, "Input/output error")
    return real(fd, data)


real_write, m.os.write = m.os.write, flaky
try:
    up3.send("V50")
finally:
    m.os.write = real_write
check("send: EIO retried", (len(calls), os.read(r, 16)), (2, b"V50"))
m.os.write = lambda *_: (_ for _ in ()).throw(OSError(m.errno.EIO, "Input/output error"))
try:
    try:
        up3.send("V50"); raised = False
    except OSError:
        raised = True
    check("send: persistent EIO raised", raised, True)
    up3.fail_safe()
    check("fail_safe swallows the error", True, True)
finally:
    m.os.write = real_write
up3.fail_safe()
check("fail_safe writes V99", os.read(r, 16), b"V99")
os.close(r); os.close(w)

# --- status file: broken JSON / missing keys ---
with tempfile.TemporaryDirectory() as d:
    m.STATUS = f"{d}/status.json"
    for content, want in (("{not json", "unreadable"), ('{"time": 1}', "unexpected content")):
        with open(m.STATUS, "w") as fh:
            fh.write(content)
        try:
            m.status(); got = "ok"
        except SystemExit as e:
            got = str(e)
        check(f"status with {content!r}", want in got, True)


def test_all():
    """pytest entry point: the checks above run at import time."""
    assert failures == 0, f"{failures} FAILED"


if __name__ == "__main__":
    print("ALL OK" if not failures else f"{failures} FAILED")
    raise SystemExit(1 if failures else 0)
