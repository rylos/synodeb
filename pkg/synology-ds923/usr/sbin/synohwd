#!/usr/bin/python3 -I
"""synohwd — Synology DS923+ hardware daemon for Debian.

Takes over the hardware management DSM does on this model, using the front-panel
microcontroller (uP) on the AMD UART at MMIO 0xFEDCA000 (AMDI0020:01) and the
DS923+ drivers built into the synodeb kernel (synology_ds923,
leds-atmega1608, rtc-s35390a).

uP protocol (9600 8N1, ASCII):
  host -> uP   '4' power LED on   '5' blink   '6' off
               '7' status off  '8' green  '9' green blink  ':' orange  ';' orange blink
               '2' short beep  '3' long beep
               'V%02d' fan duty in %
               'D' read a current sensor -> reply 'D<digits>X'; '*' reads as '0'.
                   synohwd reports raw * 16.13 (unit unknown). It is NOT the fan
                   speed: the kernel measures that on GPIO 68/69.
               'R' microP id  -> reply framed '+<n><n bytes>'
               't' / 'u' disable / enable fan-fail check
               'q' / 'p' power back on / stay off after an AC loss
               's' / 'r' enable / disable scheduled power-on (RTC alarm)
               'l' / 'k' keep / drop NIC power in S5 for Wake-on-LAN
               '@' 'A' 'B' USB LED on / blink / off
               'C' reboot, '1' HARD POWER OFF: sent by the kernel at the very end of
                   reboot/poweroff, like DSM. The daemon never sends them and
                   'synohwd send' only accepts a sequence of the commands above
                   (V<nn> with exactly two digits), so 'VV11' or 'LA1' are refused.
  uP -> host   '0' power button   'a' reset button   '`' USB copy button
               'f' fan failure    'g' CPU fan failure

Fans: hwmon "synology" (platform device synology-ds923) exposes fan1_input /
fan2_input (rpm measured by the kernel), fan1_alarm / fan2_alarm (1 = stopped)
and pwm1, which this daemon keeps in sync with the duty it sends.
Fail-safe: fans at 99% when the CPU or a present disk/NVMe has no temperature
reading, while a fan has failed, and whenever the daemon exits or crashes.

Sub-commands:
  synohwd run            the daemon (systemd)
  synohwd status         sensors, fans, bays (from /run/synohwd/status.json)
  synohwd send CMD       raw uP command through the daemon, e.g. 'synohwd send 2'
"""

import errno
import glob
import json
import logging
import os
import re
import select
import shutil
import signal
import subprocess
import sys
import termios
import threading
import time
import tomllib

CONF_PATH = "/etc/synohwd.toml"
RUN_DIR = "/run/synohwd"
STATUS = f"{RUN_DIR}/status.json"
CMD_FIFO = f"{RUN_DIR}/cmd"
PLATFORM = "/sys/devices/platform/synology-ds923"
SYS_BLOCK = "/sys/block"
LEDS = "/sys/class/leds"
UP_MMIO = 0xFEDCA000            # uP UART; 0xFEDC9000 is the serial console
NOTIFY = "/usr/lib/synodeb/notify"  # synodeb notifier (/etc/synodeb/notify.conf)
NOTIFY_WAIT = 30                # s a notification may delay a poweroff

TEMP_MIN, TEMP_MAX = -20, 150   # °C, readings outside are discarded
CRIT_READINGS = 2               # consecutive readings over a shutdown threshold
FAN_ALARM_CYCLES = 2            # consecutive cycles with fanN_alarm = 1
FAN_EVENT_HOLD = 60             # s a uP 'f'/'g' event keeps fan_fail set
REPLY_TIMEOUT = 0.5             # s for a 'D' / '+' reply to complete
D_MAX = 8                       # max digits in a 'D...X' reply
FRAME_MAX = 16                  # max length of a '+' frame
SEND_TRIES = 3                  # attempts for a uP write failing with EIO/EAGAIN
NVME_CLASS = "/sys/class/nvme"

# Same fan curves as DSM 7.4 for the DS923+, as
# (threshold °C, duty %); the last threshold of each list is the shutdown one.
FAN_TABLES = {
    "high": {  # DSM "cool mode"
        "disk": [(0, 50), (41, 60), (46, 70), (50, 80), (53, 99)], "disk_shutdown": 61,
        "nvme": [(0, 50), (44, 60), (52, 70), (60, 99)], "nvme_shutdown": 70,
        "cpu": [(0, 50), (68, 70), (78, 99)], "cpu_shutdown": 95,
        "nic": [(0, 50), (72, 70), (82, 99)], "nic_shutdown": 100,
    },
    "low": {  # DSM "quiet mode" (DSM default)
        "disk": [(0, 50), (46, 60), (52, 70), (55, 80), (58, 99)], "disk_shutdown": 61,
        "nvme": [(0, 50), (48, 60), (56, 70), (64, 99)], "nvme_shutdown": 70,
        "cpu": [(0, 50), (78, 70), (86, 99)], "cpu_shutdown": 95,
        "nic": [(0, 50), (78, 70), (88, 99)], "nic_shutdown": 100,
    },
}

# Bay -> (PCI function of the SATA controller, port index) from model.dtb:
# bays 1-2 on the JMB585 behind 00:01.5, bays 3-4 on the FCH SATA behind 00:08.2.
BAYS = {1: ("00:01.5", 0), 2: ("00:01.5", 1), 3: ("00:08.2", 0), 4: ("00:08.2", 1)}

DEFAULTS = {
    "fan_mode": "low",          # low (quiet, DSM default) | high (cool) | full
    "fan_period": 20,           # s between fan adjustments (DSM period), 5..300
    "fan_min_duty": 0,          # floor for the duty from the tables, 0..99
    "hysteresis": 3,            # °C a sensor must drop below a step to slow down, 0..20
    "thermal_shutdown": True,   # power off at the DSM shutdown thresholds
    "power_button": "poweroff", # poweroff | ignore | log | command:<shell>
    "reset_button": "log",      # log | ignore | command:<shell>
    "beep_on_start": True,
    "beep_on_error": True,
    "disk_leds": True,          # green = disk present, blinks off on I/O (kernel trigger ataN)
    "led_brightness": 255,      # 1..254 dims the disk LEDs through PWM A
    "disk_standby_minutes": 0,  # >0: spin the bay disks down after this idle time (hdparm -S), max 330
    "wol": False,               # Wake-on-LAN (magic packet) on all lan* ports
    "ac_recovery": True,        # power back on after an AC loss (DSM default)
}

# Integer settings: (min, max), out of range values are clamped.
RANGES = {"fan_period": (5, 300), "fan_min_duty": (0, 99), "hysteresis": (0, 20),
          "led_brightness": (1, 255), "disk_standby_minutes": (0, 330)}
# String settings: allowed values; the buttons also take "command:<shell>".
CHOICES = {"fan_mode": ("low", "high", "full"),
           "power_button": ("poweroff", "ignore", "log"), "reset_button": ("log", "ignore")}
COMMAND_KEYS = ("power_button", "reset_button")

# uP commands 'synohwd send' and the daemon may send ('1' and 'C' never)
UP_CMD = re.compile(r"(?:V\d\d|[2-9:;DRtuqpsrlk@AB])+")

log = logging.getLogger("synohwd")


def notify(title, message="", priority="high", then=None):
    """Send an event through the synodeb notifier, in the background; `then` (argv) runs
    after it (at most NOTIFY_WAIT s later), so a poweroff does not cut the message off."""
    def run():
        if os.access(NOTIFY, os.X_OK):
            try:
                subprocess.run([NOTIFY, "-p", priority, title, message], timeout=NOTIFY_WAIT,
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, check=False)
            except (OSError, subprocess.SubprocessError) as e:
                log.warning("notify: %s", e)
        if then:
            try:
                subprocess.Popen(then)
            except OSError as e:
                log.error("%s: %s", " ".join(then), e)
    threading.Thread(target=run, daemon=True).start()


def read(path, default=None):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def write(path, value):
    try:
        with open(path, "w") as f:
            f.write(str(value))
        return True
    except OSError as e:
        log.debug("write %s: %s", path, e)
        return False


def validate_conf(raw):
    """DEFAULTS overridden by the valid values of `raw`; anything else is logged."""
    conf = dict(DEFAULTS)
    for k, v in raw.items():
        if k == "led_activity_hz":
            log.info("led_activity_hz is ignored: the kernel trigger ataN blinks the LEDs")
            continue
        if k not in DEFAULTS:
            log.warning("%s: unknown setting %r ignored", CONF_PATH, k)
            continue
        d = DEFAULTS[k]
        if type(v) is not type(d):                  # also refuses true for an integer
            log.error("%s: %s = %r, %s expected, using %r", CONF_PATH, k, v,
                      type(d).__name__, d)
            continue
        if k in RANGES:
            lo, hi = RANGES[k]
            if not lo <= v <= hi:
                c = min(hi, max(lo, v))
                log.error("%s: %s = %r outside %d..%d, using %d", CONF_PATH, k, v, lo, hi, c)
                v = c
        elif k in CHOICES and v not in CHOICES[k] \
                and not (k in COMMAND_KEYS and v.startswith("command:") and v[8:].strip()):
            log.error("%s: %s = %r not one of %s, using %r", CONF_PATH, k, v,
                      "|".join(CHOICES[k] + (("command:<shell>",) if k in COMMAND_KEYS else ())), d)
            continue
        conf[k] = v
    return conf


def load_conf():
    try:
        with open(CONF_PATH, "rb") as f:
            return validate_conf(tomllib.load(f))
    except FileNotFoundError:
        pass
    except (OSError, tomllib.TOMLDecodeError, UnicodeDecodeError) as e:
        log.error("%s: %s — using defaults", CONF_PATH, e)
    return dict(DEFAULTS)


def check_cmd(cmd):
    """Accept only a sequence of known uP commands: never '1' (hard power-off) or 'C'."""
    if not cmd:
        raise ValueError("empty command")
    if UP_CMD.fullmatch(cmd):
        return
    if "1" in re.sub(r"V\d\d", "", cmd):
        raise ValueError(f"{cmd!r}: '1' is the uP hard power-off, refused")
    raise ValueError(f"{cmd!r}: not a sequence of uP commands (V<nn> or one of 2-9:;DRtuqpsrlk@AB)")


def up_tty():
    """/dev node of the uP UART (8250 port with iomem_base 0xFEDCA000)."""
    if os.path.exists("/dev/synology-up"):
        return "/dev/synology-up"
    for t in sorted(glob.glob("/sys/class/tty/ttyS*")):
        v = read(f"{t}/iomem_base")
        try:
            if v and int(v, 16) == UP_MMIO:
                return "/dev/" + os.path.basename(t)
        except ValueError:
            pass
    raise SystemExit(f"uP UART (MMIO {UP_MMIO:#x}) not found")


class Parser:
    """uP byte stream parser, matches DSM behaviour but is loss-tolerant.

    'D<digits>X' and '+<n><data>' are only parsed while the matching query is
    pending, are bounded in length and time out after REPLY_TIMEOUT: a lost 'X'
    or a garbage length byte can never swallow the bytes that follow. A byte that
    ends a reply early is handled again as an event, so '0' (power) and 'a'
    (reset) are not lost.
    """

    def __init__(self, on_event, on_reply):
        self.on_event, self.on_reply = on_event, on_reply
        self.expect, self.expect_until = None, 0.0   # pending query: 'D' or 'R'
        self.state, self.buf, self.frame_len, self.since = None, "", 0, 0.0

    def want(self, cmd, timeout):
        self.expect, self.expect_until = cmd, time.monotonic() + timeout

    def reset(self, why=None):
        if why:
            log.debug("uP reply dropped (%s): %r", why, self.buf)
        self.state, self.buf, self.frame_len = None, "", 0

    def tick(self, now):
        if self.state and now - self.since > REPLY_TIMEOUT:
            self.reset("timeout")

    def done(self, value):
        self.reset()
        self.expect = None
        self.on_reply(value)

    def feed(self, b, now):
        self.tick(now)
        if self.state == "D":                       # 'D' <digits|*> 'X'
            if b == "X":
                self.done(self.buf)
                return
            if b.isdigit() or b == "*":
                self.buf += "0" if b == "*" else b
                if len(self.buf) > D_MAX:
                    self.reset("too long")
                return
            self.reset(f"unexpected {b!r}")         # fall through: b is an event
        elif self.state == "+len":                  # '+' <len> <data>
            n = ord(b) - 48
            if 1 <= n <= FRAME_MAX:
                self.state, self.frame_len, self.buf = "+data", n, ""
                return
            self.reset(f"bad frame length {b!r}")   # fall through: b is an event
        elif self.state == "+data":
            self.buf += b
            if len(self.buf) >= self.frame_len:
                self.done(self.buf)
            return
        pending = self.expect if now <= self.expect_until else None
        if b == "D" and pending == "D":
            self.state, self.buf, self.since = "D", "", now
        elif b == "+" and pending == "R":
            self.state, self.since = "+len", now
        else:
            self.on_event(b)


class MicroP:
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
        attr = termios.tcgetattr(self.fd)
        attr[0] = 0                                   # iflag: raw
        attr[1] = 0                                   # oflag
        attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attr[3] = 0                                   # lflag
        attr[4] = attr[5] = termios.B9600
        attr[6][termios.VMIN] = 0
        attr[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attr)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.init_common()
        log.info("microP on %s", dev)

    def init_common(self):
        self.wlock = threading.Lock()
        self.qlock = threading.Lock()
        self.reply = None
        self.reply_ev = threading.Event()
        self.parser = Parser(None, self._on_reply)

    def _on_reply(self, value):
        self.reply = value
        self.reply_ev.set()

    def send(self, cmd):
        check_cmd(cmd)
        data = cmd.encode()
        with self.wlock:
            for attempt in range(1, SEND_TRIES + 1):
                try:
                    while data:
                        data = data[os.write(self.fd, data):]
                    break
                except OSError as e:
                    if e.errno not in (errno.EIO, errno.EAGAIN, errno.EINTR) \
                            or attempt == SEND_TRIES:
                        log.error("uP write %r: %s", cmd, e.strerror)
                        raise
                    log.warning("uP write %r: %s, retrying", cmd, e.strerror)
                    time.sleep(0.1 * attempt)
            time.sleep(0.05)

    def fail_safe(self):
        """Fans at full speed, without the write lock (may be called from a dying thread).
        A single short write() on a tty is not interleaved with another one."""
        try:
            os.write(self.fd, b"V99")
            log.info("fan duty 99% (fail-safe)")
        except OSError as e:
            log.error("uP write 'V99' (fail-safe): %s", e.strerror)

    def query(self, cmd, timeout=1.0):
        with self.qlock:
            self.reply_ev.clear()
            self.reply = None
            self.parser.want(cmd[0], timeout)
            self.send(cmd)
            return self.reply if self.reply_ev.wait(timeout) else None

    def reader(self, on_event):
        self.parser.on_event = on_event
        empty = 0
        while True:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            now = time.monotonic()
            if not r:
                self.parser.tick(now)
                continue
            data = os.read(self.fd, 64)
            if not data:                  # readable but nothing: hangup
                empty += 1
                if empty > 50:
                    raise OSError("uP UART: end of file")
                time.sleep(0.02)
                continue
            empty = 0
            for b in data.decode("latin-1"):
                self.parser.feed(b, now)


def hwmon_by_name(name):
    out = []
    for d in glob.glob("/sys/class/hwmon/hwmon*"):
        if read(f"{d}/name") == name:
            out.append(d)
    return out


def hwmon_temp(d, label=None):
    """Highest plausible temp*_input (or the one with that label) of a hwmon dir, °C."""
    best = None
    for inp in glob.glob(f"{d}/temp*_input"):
        if label and read(inp.replace("_input", "_label")) != label:
            continue
        v = read(inp)
        if v and v.lstrip("-").isdigit():
            t = int(v) / 1000
            if not TEMP_MIN <= t <= TEMP_MAX:
                log.debug("%s: implausible %s °C ignored", inp, t)
                continue
            best = t if best is None else max(best, t)
    return best


def bay_disks():
    """{bay: ('sdX', N)} following the ATA port topology of model.dtb; N is the libata
    port number (sysfs .../ataN/, also the name of its LED trigger "ataN")."""
    found = {}
    for blk in glob.glob(f"{SYS_BLOCK}/sd*"):
        path = os.path.realpath(f"{blk}/device")
        m = re.search(r"/(0000:[0-9a-f:.]+)/(?:0000:[0-9a-f:.]+/)*.*?/ata(\d+)/", path)
        if not m:
            continue
        ata = int(m.group(2))
        for bay, (root, port) in BAYS.items():
            if f"/0000:{root}/" not in path:
                continue
            ctrl = path.split(f"/ata{ata}/")[0]
            ports = sorted(int(os.path.basename(p)[3:]) for p in glob.glob(f"{ctrl}/ata*")
                           if os.path.basename(p)[3:].isdigit())
            if ata in ports and ports.index(ata) == port:
                found[bay] = (os.path.basename(blk), ata)
    return found


def bay_block_devices():
    """{bay: 'sdX'}"""
    return {bay: dev for bay, (dev, _) in bay_disks().items()}


def led_target(present, dev, ata, dev_state):
    """What a bay's LEDs should show: (green trigger, green brightness, orange brightness).

    trigger "ataN": the kernel keeps the green LED on and blinks it off on every completed
    ATA command of that port (CONFIG_ATA_LEDS); "none" = fixed brightness.
    """
    if not present:
        return ("none", 0, 0)                    # empty bay
    if not dev:
        return ("none", 1, 0)                    # disk present, not yet probed: green steady
    if dev_state not in (None, "running"):
        return ("none", 0, 1)                    # error: orange
    return (f"ata{ata}" if ata is not None else "none", 1, 0)


def disk_asleep(dev):
    """True if hdparm -C (CHECK POWER MODE, does not spin up) says standby/sleeping.

    None when it cannot tell (no hdparm, error): the caller reads the disk anyway.
    """
    if not shutil.which("hdparm"):
        return None
    try:
        r = subprocess.run(["hdparm", "-C", f"/dev/{dev}"], capture_output=True,
                           text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.search(r"drive state is:\s*(\S+)", r.stdout)
    if not m:
        return None
    return m.group(1) in ("standby", "sleeping")


def sensors(bays, last_disk=None):
    """Temperatures; a spun-down bay disk keeps its last known value (no wake-up)."""
    last_disk = {} if last_disk is None else last_disk
    s = {"cpu": None, "disk": {}, "nvme": {}, "nic": {}, "asleep": []}
    for d in hwmon_by_name("k10temp"):
        t = hwmon_temp(d, "Tctl")
        s["cpu"] = hwmon_temp(d) if t is None else t
    for bay, dev in bays.items():
        key = f"bay{bay}"
        if disk_asleep(dev):
            s["asleep"].append(key)
            if last_disk.get(key) is not None:
                s["disk"][key] = last_disk[key]
            continue
        s["disk"][key] = None                   # no drivetemp: no reading (fail-safe)
        for d in glob.glob(f"{SYS_BLOCK}/{dev}/device/hwmon/hwmon*"):   # drivetemp
            t = hwmon_temp(d)
            s["disk"][key] = t
            if t is not None:
                last_disk[key] = t
    nvme_hw = hwmon_by_name("nvme")
    for d in nvme_hw:
        dev = os.path.basename(os.path.realpath(f"{d}/device"))
        t = hwmon_temp(d, "Composite")
        s["nvme"][dev] = hwmon_temp(d) if t is None else t
    # a controller without its hwmon has no reading (fail-safe)
    ctrls = sorted(os.path.basename(c) for c in glob.glob(f"{NVME_CLASS}/nvme*"))
    for c in [c for c in ctrls if c not in s["nvme"]][:max(0, len(ctrls) - len(nvme_hw))]:
        s["nvme"][c] = None
    for name in ("atlantic", "aqc", "r8169"):
        for d in hwmon_by_name(name):
            dev = os.path.basename(os.path.realpath(f"{d}/device"))
            t = hwmon_temp(d)
            if t is not None:
                s["nic"][dev] = t
    return s


class Fan:
    def __init__(self, conf):
        self.conf = conf
        self.level = {}   # sensor class -> current step index (for hysteresis)
        self.missing = frozenset()   # sensors without a reading, logged on change
        self.failed = False

    def step(self, cls, temp, table):
        idx = 0
        for i, (thr, _) in enumerate(table):
            if temp >= thr:
                idx = i
        cur = self.level.get(cls, 0)
        # slow down only past each step's threshold minus the hysteresis margin
        if idx < cur:
            for i in range(min(cur, len(table) - 1), idx, -1):
                if temp > table[i][0] - self.conf["hysteresis"]:
                    idx = i
                    break
        self.level[cls] = idx
        return table[idx][1]

    def duty(self, s, fan_fail=False):
        """(duty %, critical reason or None). 99 in "full" mode, while a fan has failed
        and when the CPU or a present disk/NVMe has no reading."""
        t = FAN_TABLES.get(self.conf["fan_mode"], FAN_TABLES["low"])
        duty, critical = self.conf["fan_min_duty"], None
        groups = {
            "disk": [v for v in s["disk"].values() if v is not None],
            "nvme": [v for v in s["nvme"].values() if v is not None],
            "cpu": [s["cpu"]] if s["cpu"] is not None else [],
            "nic": [v for v in s["nic"].values() if v is not None],
        }
        for cls, temps in groups.items():
            if not temps:
                continue
            hot = max(temps)
            duty = max(duty, self.step(cls, hot, t[cls]))
            if hot >= t[f"{cls}_shutdown"]:
                critical = f"{cls} {hot:.0f}°C >= {t[cls + '_shutdown']}°C"
        missing = frozenset((["cpu"] if s["cpu"] is None else [])
                            + [f"{cls} {k}" for cls in ("disk", "nvme")
                               for k, v in s[cls].items() if v is None])
        if missing != self.missing:
            if missing:
                log.warning("no temperature reading from %s: fans at 99%%",
                            ", ".join(sorted(missing)))
            else:
                log.info("all temperature sensors readable again")
            self.missing = missing
        if fan_fail and not self.failed:
            log.warning("fan failure: fans at 99% while it lasts")
        self.failed = fan_fail
        if self.conf["fan_mode"] == "full" or missing or fan_fail:
            duty = 99
        return min(duty, 99), critical


class Thermal:
    """Shutdown only when a threshold is exceeded in CRIT_READINGS readings in a row."""

    def __init__(self):
        self.streak = 0

    def update(self, critical):
        self.streak = self.streak + 1 if critical else 0
        return self.streak >= CRIT_READINGS


class FanHealth:
    """fan_fail from the kernel alarms (2 cycles in a row; None = unreadable tach counts
    too) and the uP 'f'/'g' events."""

    def __init__(self):
        self.streak = 0
        self.last_event = None

    def event(self, now):
        self.last_event = now

    def update(self, alarms, now):
        self.streak = self.streak + 1 if any(a != 0 for a in alarms) else 0
        recent = self.last_event is not None and now - self.last_event < FAN_EVENT_HOLD
        return self.streak >= FAN_ALARM_CYCLES or recent


def synology_hwmon():
    return sorted(glob.glob(f"{PLATFORM}/hwmon/hwmon*"))


def int_or_none(v):
    return int(v) if v is not None and v.lstrip("-").isdigit() else None


def fans_of(hwmons):
    """Both fans of each hwmon dir, always: an unreadable tach (attribute missing = no
    tach GPIO/IRQ, ENODATA = not measured yet after probe) gives alarm None, which
    FanHealth counts as a failure only once it lasts FAN_ALARM_CYCLES cycles."""
    return [{"fan": n, "rpm": int_or_none(read(f"{d}/fan{n}_input")),
             "alarm": int_or_none(read(f"{d}/fan{n}_alarm"))}
            for d in hwmons for n in (1, 2)]


class Daemon:
    def __init__(self):
        self.conf = load_conf()
        self.up = MicroP(up_tty())
        self.fan = Fan(self.conf)
        self.thermal = Thermal()
        self.fan_health = FanHealth()
        self.state = {"fan_duty": None, "fans": [], "fan_fail": False,
                      "up_current": None, "status_led": None, "events": []}
        self.bays = {}
        self.last_disk = {}
        self.stop = threading.Event()
        self.last_power_press = 0
        self.shutting_down = False

    def event(self, b):
        if b == "0":
            now = time.monotonic()
            if now - self.last_power_press < 2:
                return
            self.last_power_press = now
            self.note("power button")
            self.button(self.conf["power_button"], ["systemctl", "poweroff"], "power button")
        elif b == "a":
            self.note("reset button")
            self.button(self.conf["reset_button"], None)
        elif b == "`":
            self.note("usb copy button")
        elif b in ("f", "g"):
            self.note("fan failure" if b == "f" else "cpu fan failure")
            if not self.state["fan_fail"]:
                notify("fan failure", "reported by the microcontroller "
                       f"({'fan' if b == 'f' else 'CPU fan'})", "urgent")
            self.fan_health.event(time.monotonic())
            self.state["fan_fail"] = True
            if self.conf["beep_on_error"]:
                self.up.send("3")
        elif b.strip():
            log.debug("uP byte %r", b)

    def button(self, action, default_cmd, name=None):
        if action == "ignore" or action == "log":
            return
        if action.startswith("command:"):
            try:
                subprocess.Popen(action[8:], shell=True)
            except OSError as e:
                log.error("%s: %s", action, e)
        elif default_cmd:
            self.up.send("3")          # long beep, as DSM on the power button
            self.up.send("5")          # power LED blinks while shutting down
            notify(f"shutdown: {name}", f"{name}: {' '.join(default_cmd)}", "high",
                   then=default_cmd)

    def note(self, what):
        log.info("event: %s", what)
        self.state["events"] = (self.state["events"] + [[time.strftime("%F %T"), what]])[-20:]

    def raid_degraded(self):
        md = read("/proc/mdstat", "")
        return bool(re.search(r"\[[U_]*_[U_]*\]", md))

    def set_status(self, code):
        if self.state["status_led"] != code:
            self.up.send(code)
            self.state["status_led"] = code

    def publish_pwm(self):
        if self.state["fan_duty"] is None:
            return
        for d in synology_hwmon():
            write(f"{d}/pwm1", round(self.state["fan_duty"] * 255 / 99))

    def read_fans(self):
        """[{fan, rpm, alarm}] from hwmon "synology" (tach GPIO 68/69, measured by the kernel)."""
        return fans_of(synology_hwmon())

    def read_current(self):
        r = self.up.query("D")
        if r and r.isdigit():
            # raw * 16.13, unit unknown
            v = int(r)
            self.state["up_current"] = 16 * v + 13 * v // 100

    def update_fan_fail(self):
        fans = self.read_fans()
        self.state["fans"] = fans
        was = self.state["fan_fail"]
        fail = self.fan_health.update([f["alarm"] for f in fans], time.monotonic())
        if fail and not was:
            self.note("fan stopped or unreadable (hwmon)")
            notify("fan failure", "fan stopped or unreadable (hwmon): "
                   + ", ".join(f"fan{f['fan']} {'?' if f['rpm'] is None else f['rpm']} rpm"
                               for f in fans), "urgent")
            if self.conf["beep_on_error"]:
                self.up.send("3")
        elif was and not fail:
            self.note("fans ok")
            notify("fans ok again", "", "normal")
        self.state["fan_fail"] = fail

    def control_loop(self):
        while not self.stop.is_set():
            self.bays = bay_block_devices()
            s = sensors(self.bays, self.last_disk)
            self.update_fan_fail()
            duty, critical = self.fan.duty(s, self.state["fan_fail"])
            if duty != self.state["fan_duty"]:
                self.up.send(f"V{duty:02d}")
                log.info("fan duty %d%%", duty)
                self.state["fan_duty"] = duty
            self.publish_pwm()
            self.read_current()
            problem = self.state["fan_fail"] or self.raid_degraded()
            self.set_status(";" if problem else "8")
            if critical:
                log.warning("over the shutdown threshold: %s", critical)
            if self.thermal.update(critical) and self.conf["thermal_shutdown"] \
                    and not self.shutting_down:
                self.shutting_down = True
                self.note(f"thermal shutdown: {critical}")
                self.up.send("3")
                notify("thermal shutdown", critical, "urgent", then=["systemctl", "poweroff"])
            self.write_status(s)
            self.stop.wait(self.conf["fan_period"])

    def write_status(self, s):
        bays = {}
        for n in range(1, 5):
            bays[n] = {"present": read(f"{PLATFORM}/bay{n}_present") == "1",
                       "power": read(f"{PLATFORM}/bay{n}_power") == "1",
                       "device": self.bays.get(n)}
        data = {"time": time.strftime("%F %T"), "epoch": int(time.time()),
                "period": self.conf["fan_period"], "sensors": s, "bays": bays, **self.state}
        tmp = STATUS + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=1)
        os.replace(tmp, STATUS)

    def apply_standby(self):
        minutes = int(self.conf["disk_standby_minutes"])
        if minutes <= 0:
            return
        # hdparm -S: 1..240 = n*5 s, 241..251 = (n-240)*30 min
        val = min(240, minutes * 12) if minutes <= 20 else min(251, 240 + -(-minutes // 30))
        for dev in bay_block_devices().values():
            if run_quiet(["hdparm", "-q", "-S", str(val), f"/dev/{dev}"]):
                log.info("standby %s after %d min", dev, minutes)

    def apply_wol(self):
        """WOL on or off on every lan* port: off too, the firmware may leave it on."""
        mode = "g" if self.conf["wol"] else "d"
        for n in glob.glob("/sys/class/net/lan*"):
            run_quiet(["ethtool", "-s", os.path.basename(n), "wol", mode])

    def ac_recovery_cmd(self):
        return "q" if self.conf["ac_recovery"] else "p"

    def prepare_poweroff(self):
        """Tell the uP what may wake the NAS once it is off (like DSM does)."""
        alarm = False
        for rtc in glob.glob("/sys/class/rtc/rtc*"):
            if os.path.basename(os.path.realpath(f"{rtc}/device")).endswith("-0030"):  # S-35390A
                alarm = bool(read(f"{rtc}/wakealarm"))
        self.up.send("s" if alarm else "r")
        self.up.send("l" if self.conf["wol"] else "k")
        self.up.send(self.ac_recovery_cmd())
        log.info("power-off: scheduled power-on %s, WOL %s, AC recovery %s",
                 alarm, self.conf["wol"], self.conf["ac_recovery"])

    def led(self, bay, color, value):
        write(f"{LEDS}/syno:{color}:disk{bay}/brightness", value)

    def apply_led(self, bay, target, on):
        """Set one bay. The trigger is written before the brightness: writing 0 to
        'brightness' detaches any trigger (Linux LED class), a non-zero value keeps it,
        so dimming (led_brightness < 255) also works with the activity trigger."""
        trig, green, orange = target
        g = f"{LEDS}/syno:green:disk{bay}"
        if trig != "none" and trig not in read(f"{g}/trigger", "").replace("[", " ").replace("]", " ").split():
            log.warning("LED trigger %s missing (kernel without CONFIG_ATA_LEDS?): "
                        "bay %d green steady", trig, bay)
            trig = "none"
        write(f"{g}/trigger", trig)
        if trig == "none" or on != 255:
            self.led(bay, "green", on if green else 0)
        self.led(bay, "orange", on if orange else 0)

    def led_step(self, shown, on):
        disks = bay_disks()
        for bay in range(1, 5):
            dev, ata = disks.get(bay, (None, None))
            present = read(f"{PLATFORM}/bay{bay}_present") == "1"
            st = read(f"{SYS_BLOCK}/{dev}/device/state") if dev else None
            target = led_target(present, dev, ata, st)
            if shown.get(bay) != target:
                self.apply_led(bay, target, on)
                shown[bay] = target

    def led_loop(self):
        """Disk LEDs at 1 Hz: the I/O blinking is done by the kernel (trigger ataN)."""
        on = self.conf["led_brightness"]
        shown = {}
        while not self.stop.is_set():
            self.led_step(shown, on)
            self.stop.wait(1)

    def cmd_loop(self):
        try:
            os.mkfifo(CMD_FIFO, 0o600)
        except FileExistsError:
            pass
        # O_RDWR: the daemon is a writer too, so the FIFO never hits EOF and
        # 'synohwd send' (O_NONBLOCK) only gets ENXIO when the daemon is gone
        fd = os.open(CMD_FIFO, os.O_RDWR)
        buf = b""
        while not self.stop.is_set():
            r, _, _ = select.select([fd], [], [], 1.0)
            if not r:
                continue
            buf += os.read(fd, 256)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.decode("latin-1").strip()
                if not cmd:
                    continue
                try:
                    self.up.send(cmd)
                    log.info("sent %r", cmd)
                except ValueError as e:
                    log.warning("%s", e)
            buf = buf[-256:]

    def start_thread(self, target, *args):
        def run():
            try:
                target(*args)
            except BaseException:
                log.exception("thread %s died, exiting so that systemd restarts the daemon",
                              target.__name__)
                self.up.fail_safe()          # os._exit skips the finally of run()
                remove_status()
                logging.shutdown()
                os._exit(1)
        threading.Thread(target=run, name=target.__name__, daemon=True).start()

    def on_signal(self, signum, _frame):
        log.info("%s: stopping", signal.Signals(signum).name)
        self.stop.set()

    def run(self):
        os.makedirs(RUN_DIR, exist_ok=True)
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(sig, self.on_signal)
        try:
            self.start_thread(self.up.reader, self.event)
            mpid = self.up.query("R")
            log.info("microP id %r", mpid)
            # boot finished, like DSM: fan check on, power LED steady, status green, long beep
            self.up.send("u")
            self.up.send("4")
            self.set_status("8")
            if self.conf["beep_on_start"]:
                self.up.send("3")
            self.up.send(self.ac_recovery_cmd())
            self.apply_standby()
            self.apply_wol()
            self.start_thread(self.cmd_loop)
            if self.conf["disk_leds"]:
                self.start_thread(self.led_loop)
            self.control_loop()
        finally:
            self.shutdown()

    def shutdown(self):
        """On every exit, also a crash: fans at full speed first, then the power-off setup.
        'C' (reboot) and '1' (power off) are sent by the kernel at the very end."""
        self.up.fail_safe()
        for step in (self.prepare_poweroff,
                     lambda: self.up.send("t"),   # fan-fail check off: fans stop at power-off
                     lambda: self.up.send("5"),   # shutting down: power LED blinks
                     lambda: self.up.send("7")):
            try:
                step()
            except OSError as e:
                log.error("shutdown: %s", e)
        for bay in range(1, 5):
            self.led(bay, "green", 0)            # 0 also detaches the ataN trigger
            self.led(bay, "orange", 0)
        remove_status()


def run_quiet(argv):
    """Run a helper (hdparm, ethtool); False with a log line if it is missing or fails."""
    try:
        r = subprocess.run(argv, stdin=subprocess.DEVNULL, check=False, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        log.warning("%s: %s", argv[0], e)
        return False
    if r.returncode:
        log.warning("%s: exit status %d", " ".join(argv), r.returncode)
    return r.returncode == 0


def remove_status():
    for p in (STATUS, STATUS + ".tmp"):
        try:
            os.unlink(p)
        except OSError:
            pass


def status():
    try:
        with open(STATUS) as f:
            d = json.load(f)
    except OSError:
        raise SystemExit("synohwd is not running (no /run/synohwd/status.json)")
    except ValueError as e:                     # JSONDecodeError, bad UTF-8
        raise SystemExit(f"{STATUS}: unreadable ({e})")
    try:
        print_status(d)
    except (KeyError, TypeError, AttributeError, ValueError) as e:
        raise SystemExit(f"{STATUS}: unexpected content ({e!r})")


def deg(v):
    return "?" if v is None else f"{v} °C"


def print_status(d):
    s = d["sensors"]
    age = time.time() - d.get("epoch", time.time())
    stale = "  (STALE: the daemon is not updating)" if age > 3 * d.get("period", 20) + 10 else ""
    print(f"updated      {d['time']}{stale}")
    print(f"fans         duty {d['fan_duty']}%{'  FAILURE' if d['fan_fail'] else ''}")
    for f in d.get("fans", []):
        print(f"fan {f['fan']}        {f['rpm'] if f['rpm'] is not None else '?'} rpm"
              f"{'  STOPPED' if f['alarm'] == 1 else ''}")
    if d.get("up_current") is not None:
        print(f"uP current   {d['up_current']} (unit unknown)")
    print(f"CPU          {deg(s['cpu'])}")
    for k, v in s["nvme"].items():
        print(f"NVMe {k:7} {deg(v)}")
    for k, v in s["nic"].items():
        print(f"NIC {k}  {deg(v)}")
    for n, b in d["bays"].items():
        t = s["disk"].get(f"bay{n}")
        sleep = "  standby" if f"bay{n}" in s.get("asleep", []) else ""
        print(f"bay {n}        {'disk' if b['present'] else 'empty'}  "
              f"power {'on' if b['power'] else 'off'}  {b['device'] or ''}  "
              f"{deg(t) if b['device'] else ''}{sleep}")
    for when, what in d["events"][-5:]:
        print(f"event        {when} {what}")


def send(cmd):
    try:
        check_cmd(cmd)
    except ValueError as e:
        raise SystemExit(f"refused: {e}")
    try:
        fd = os.open(CMD_FIFO, os.O_WRONLY | os.O_NONBLOCK)
    except FileNotFoundError:
        raise SystemExit("synohwd is not running (no FIFO /run/synohwd/cmd)")
    except OSError as e:
        if e.errno == errno.ENXIO:
            raise SystemExit("synohwd is not running (nobody reads /run/synohwd/cmd)")
        raise SystemExit(f"{CMD_FIFO}: {e.strerror}")
    try:
        os.write(fd, (cmd + "\n").encode())
    except BlockingIOError:
        raise SystemExit("synohwd is busy (command FIFO full), try again")
    except OSError as e:
        raise SystemExit(f"{CMD_FIFO}: {e.strerror}")
    finally:
        os.close(fd)


def main():
    logging.basicConfig(level=os.environ.get("SYNOHWD_LOG", "INFO"),
                        format="%(levelname)s %(message)s")
    cmd = sys.argv[1] if len(sys.argv) > 1 else "run"
    if cmd == "run":
        Daemon().run()
    elif cmd == "status":
        status()
    elif cmd == "send" and len(sys.argv) == 3:
        send(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
