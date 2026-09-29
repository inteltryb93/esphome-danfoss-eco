#!/usr/bin/env python3
"""Timed scenario tests for the Danfoss eTRV controller (use a VERBOSE build for the full phase breakdown).

Every command is followed through the device log and timed from the API send to the moment the eTRV
itself reports the new set point (read-back after the write):

  send -> ctrl (component received it) -> req (link requested) -> open (link up, after failed opens)
       -> pin (PIN accepted) -> write_rsp (set point written) -> delivered (read back from the eTRV)
       -> close (link closed)

Scenarios (--scenarios, comma separated, run in this order):
  parallel  one set point change on ALL thermostats at once; total = until the last one is read back
  cold      reboot the controller, then one command right away (first link after boot)
  ab        write to A, then B, then straight back to A ("hot") - all thermostat pairs
  same      two commands to the same thermostat back to back (the second on a warm eTRV)
  double    a second value 150 ms after the first (the first must be superseded, never written)
  readonly  re-send the current set point to all at once: read-only links (nothing written)
  mixed     writes to half of the thermostats and read-only links to the others, simultaneously
  txab      Bluetooth TX power A/B: read-only links to every thermostat, continuously, with the TX
            power alternating between +9 and +20 dBm every --txab-minutes (needs a test firmware with
            template buttons "BLE TX 9 dBm" / "BLE TX 20 dBm")

Set points move by --delta around the value each thermostat had at the start and are restored at the
end (also on Ctrl-C). Commands are not sent close to the minute a Home Assistant automation writes the
set points (hh:m0, --avoid-automation); a set point written by another client is followed (the test
restores to it). A lost API connection is re-established; commands in flight then are not timed.
The report ends with the radio statistics of the run: connection attempts, how many connected, and
how long the radio was idle while work was pending (the cost of retry back-off).

Usage: scenario_test.py [--host sterownik-grzejnika.local] --out run.log [--scenarios parallel,cold,ab]
                        [--repeat 3] [--delta 0.5] [--timeout 600]
"""
import argparse
import asyncio
import datetime
import re
import signal
import statistics
import time

from aioesphomeapi import APIClient, ButtonInfo, ClimateInfo, ClimateState, LogLevel, SensorInfo, SensorState

ANSI = re.compile(r"\x1b\[[0-9;]*m")
ANOMALY = re.compile(r"WATCHDOG|request timeout|giving up|protocol error|wrong secret|unexpected (read|write)|"
                     r"ALREADY_OPEN|dropping the requested|lost with \d+ unanswered|implausible|unusable data|"
                     r"forgetting GATT|Guru|panic|abort")
PHASES = ("ctrl", "req", "open", "pin", "write_rsp", "delivered", "close")


def now():
    return time.monotonic()


def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def link_stats(path, since=None):
    """Radio statistics from the device log written by this test: connection attempts (ESPHome's
    "Connecting" -> our "connect"/"failed to open"), and the time during which some eTRV had a
    transaction pending while the radio did nothing (no connection attempt, no link)."""
    def secs(line):
        m = re.match(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\]", line)
        return None if not m else int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 10 ** len(m.group(4))
    pending, pend_iv, busy_iv, ok_t = {}, [], [], []
    fails, attempt, link, day, last = 0, None, None, 0, None
    for line in open(path, errors="replace"):
        t = secs(line)
        if t is None:
            continue
        if last is not None and t < last - 3600:
            day += 86400
        last = t
        t += day
        if since is not None and t < since:
            continue
        if re.search(r"\[\d\] \[[0-9A-F:]{17}\] 0x0\d Connecting", line):
            attempt = t
            continue
        m = re.search(r"\[(\w+_thermostat)\] (.*)", line)
        if not m:
            continue
        name, body = m.group(1), m.group(2)
        if body.startswith("requesting BLE link (attempt 1,") and name not in pending:
            pending[name] = t
        elif body.startswith("connect, conn_id"):
            if attempt is not None:
                ok_t.append(t - attempt)
            link, attempt = (name, attempt if attempt is not None else t), None
        elif body.startswith("failed to open"):
            if attempt is not None:
                busy_iv.append((attempt, t))
                fails += 1
            attempt = None
        elif body.startswith("closing link"):
            if link and link[0] == name:
                busy_iv.append((link[1], t))
                link = None
            if ("transaction complete" in body or "nothing pending" in body) and name in pending:
                pend_iv.append((pending.pop(name), t))

    def union(iv):
        out = []
        for a, b in sorted(iv):
            if out and a <= out[-1][1]:
                out[-1][1] = max(out[-1][1], b)
            else:
                out.append([a, b])
        return out
    pend, busy = union(pend_iv), union(busy_iv)
    total = sum(b - a for a, b in pend)
    idle = sum((b - a) - sum(max(0.0, min(b, y) - max(a, x)) for x, y in busy) for a, b in pend)
    n = len(ok_t) + fails
    ok_t.sort()
    return (f"connection attempts {n}: connected {len(ok_t)} ({len(ok_t) / max(1, n):.0%}), connect time median "
            f"{statistics.median(ok_t) if ok_t else 0:.1f} s p90 {ok_t[int(len(ok_t) * 0.9)] if ok_t else 0:.1f} s | work pending "
            f"{total:.0f} s, radio idle meanwhile {idle:.0f} s ({idle / max(1.0, total):.0%})")


def attempts_by_tx_level(path):
    """Connection attempts per eTRV and per Bluetooth TX power level (scenario txab marks)."""
    level, attempt, out = None, False, {}
    for line in open(path, errors="replace"):
        m = re.search(r"\[test\] txab: BLE TX power (\d+) dBm", line)
        if m:
            level = int(m.group(1))
            continue
        if level is None:
            continue
        if re.search(r"\[\d\] \[[0-9A-F:]{17}\] 0x0\d Connecting", line):
            attempt = True
            continue
        m = re.search(r"\[(\w+)_thermostat\] (connect, conn_id|failed to open)", line)
        if m and attempt:
            r = out.setdefault((level, m.group(1)), [0, 0])
            r[0] += m.group(2).startswith("connect")
            r[1] += 1
            attempt = False
    lines = []
    for lvl in sorted({k[0] for k in out}):
        ok = sum(v[0] for k, v in out.items() if k[0] == lvl)
        n = sum(v[1] for k, v in out.items() if k[0] == lvl)
        per = ", ".join(f"{k[1]} {v[0]}/{v[1]}" for k, v in sorted(out.items()) if k[0] == lvl)
        lines.append(f"TX {lvl:2d} dBm: {ok}/{n} attempts connected ({ok / max(1, n):.0%}) | {per}")
    return lines


class Cmd:
    def __init__(self, name, value, kind, scenario):
        self.name, self.value, self.kind, self.scenario = name, value, kind, scenario
        self.t_send = now()
        self.t = {}
        self.attempts = 0
        self.failed_opens = 0
        self.writes = 0
        self.link_age = None
        self.superseded = None
        self.no_link = False     # the component skipped it (value equal to a fresh read)
        self.written = []        # every set point written while this command was active
        self.done = asyncio.Event()

    def mark(self, key, t=None, first=True):
        if first and key in self.t:
            return
        self.t[key] = t if t is not None else now()

    def rel(self, key):
        return None if key not in self.t else self.t[key] - self.t_send


class Runner:
    def __init__(self, args):
        self.args = args
        self.cli = None
        self.climates = {}
        self.restart_key = None
        self.tx_buttons = {}
        self.states = {}
        self.baseline = {}
        self.active = {}
        self.sent = {}  # name -> [(value, time)] of the test's own recent commands
        self.last_link_end = {}  # name -> time of the last "closing link" (any origin)
        self.results = []
        self.anomalies = []
        self.logf = open(args.out, "a", buffering=1)
        self.connected = asyncio.Event()
        self.stop = asyncio.Event()
        self.expect_drop = False  # a deliberate disconnect (reboot test): do not reconnect on our own
        self.reported = False
        self.reconnecting = None
        self.api_gaps = []        # (lost, back) of unexpected API connection losses
        self.diag_keys = {}       # sensor key -> object_id of the optional WiFi diagnostics
        self.wifi_signal = []
        self.wifi_disconnects = None

    # ---------------------------------------------------------------- connection / log parsing
    async def connect(self):
        for attempt in range(60):
            try:
                self.cli = APIClient(self.args.host, 6053, None)
                await self.cli.connect(login=True, on_stop=self.on_stop)
                ents, _ = await self.cli.list_entities_services()
                self.climates = {e.object_id: e for e in ents if isinstance(e, ClimateInfo)}
                self.diag_keys = {e.key: e.object_id for e in ents if isinstance(e, SensorInfo) and e.object_id in ("wifi_signal", "wifi_disconnects")}
                self.restart_key = next((e.key for e in ents if isinstance(e, ButtonInfo) and e.object_id.endswith("restart") and "safe" not in e.object_id), None)
                # optional test-firmware buttons "BLE TX 9 dBm" / "BLE TX 20 dBm" (scenario txab)
                self.tx_buttons = {lvl: e.key for e in ents if isinstance(e, ButtonInfo) for lvl in (9, 20) if e.object_id == f"ble_tx_{lvl}_dbm"}
                self.cli.subscribe_states(self.on_state)
                self.cli.subscribe_logs(self.on_log, log_level=LogLevel(self.args.level), dump_config=False)
                self.connected.set()
                return
            except Exception as exc:  # noqa: BLE001
                self.note(f"connect attempt {attempt + 1} failed: {exc!r}")
                await asyncio.sleep(2)
        raise RuntimeError("could not connect")

    async def on_stop(self, expected):
        self.connected.clear()
        self.note("API connection lost")
        # log lines are lost until the connection is back: commands in flight cannot be timed
        for c in self.active.values():
            if not c.done.is_set():
                c.superseded = c.superseded or "API connection lost"
                c.done.set()
        if not expected and not self.expect_drop and self.reconnecting is None:
            self.reconnecting = asyncio.get_running_loop().create_task(self.reconnect())

    async def reconnect(self):
        lost = now()
        try:
            await asyncio.sleep(1)
            await self.connect()
            self.api_gaps.append((lost, now()))
            self.note(f"API reconnected after {now() - lost:.1f} s")
        finally:
            self.reconnecting = None

    def note(self, text):
        line = f"{stamp()} {text}"
        print(line, flush=True)
        self.logf.write(f"[{stamp()}][test] {text}\n")

    def on_state(self, st):
        if isinstance(st, SensorState) and st.key in self.diag_keys and not st.missing_state:
            if self.diag_keys[st.key] == "wifi_signal":
                self.wifi_signal.append(st.state)
            else:
                n = int(st.state)
                if self.wifi_disconnects is not None and n > self.wifi_disconnects:
                    self.note(f"device WiFi disconnects: {self.wifi_disconnects} -> {n}")
                self.wifi_disconnects = n
            return
        if isinstance(st, ClimateState):
            for name, e in self.climates.items():
                if e.key == st.key:
                    self.states[name] = st

    def on_log(self, msg):
        t = now()
        text = ANSI.sub("", msg.message.decode("utf-8", "replace") if isinstance(msg.message, (bytes, bytearray)) else msg.message)
        self.logf.write(f"[{stamp()}]{text}\n")
        if ANOMALY.search(text) and "fake" not in text:
            self.anomalies.append(f"{stamp()} {text.strip()[:200]}")
        m = re.search(r"\[(\w+_thermostat)\] (.*)$", text)
        if not m:
            return
        name, body = m.group(1), m.group(2)
        if body.startswith("target temperature change:") and name in self.baseline:
            # A set point this test did not send (Home Assistant automation): it owns the set point,
            # so the test restores to that value from now on - even between two test operations.
            m2 = re.search(r"-> ([\d.]+) C", body)
            if m2:
                v = float(m2.group(1))
                ours = any(abs(v - sv) < 0.01 and t - st < 60 for sv, st in self.sent.get(name, []))
                if not ours and abs(v - self.baseline[name]) > 0.01:
                    self.note(f"{name}: another client set {v:.1f} - the test restores to it from now on")
                    self.baseline[name] = v
        if body.startswith("closing link"):
            self.last_link_end[name] = t
        c = self.active.get(name)
        if c is None or c.done.is_set():
            return
        if body.startswith("target temperature change:"):
            m2 = re.search(r"-> ([\d.]+) C", body)
            if m2 and abs(float(m2.group(1)) - c.value) > 0.01 and "ctrl" in c.t:
                c.superseded = f"another client set {m2.group(1)}"
                # the other client (Home Assistant automation) owns the set point: follow it
                self.baseline[name] = float(m2.group(1))
                c.done.set()
                return
            c.mark("ctrl", t)
        elif body.startswith("target temperature unchanged"):
            c.mark("ctrl", t)
            c.mark("delivered", t)
            c.no_link = True
            c.done.set()
        elif body.startswith("requesting BLE link"):
            c.mark("req", t)
            c.attempts += 1
        elif body.startswith("failed to open"):
            c.failed_opens += 1
        elif body.startswith("open OK"):
            c.mark("open", t, first=False)
        elif body.startswith("pin OK"):
            c.mark("pin", t, first=False)
        elif body.startswith("writing set point"):
            c.writes += 1
            c.mark("write", t, first=False)
            m2 = re.search(r"-> ([\d.]+) C", body)
            if m2:
                c.written.append(float(m2.group(1)))
        elif body.startswith("write rsp: handle=0x2d status=0000"):
            c.mark("write_rsp", t, first=False)
        elif body.startswith("device already has the requested set point"):
            c.mark("write_rsp", t, first=False)
            self.delivered(c, t)
        elif body.startswith("TEMP PROCESSED:"):
            m2 = re.search(r"target=([\d.]+)", body)
            if m2 and abs(float(m2.group(1)) - c.value) < 0.01 and ("write_rsp" in c.t or c.kind == "poke"):
                self.delivered(c, t)
        elif body.startswith("closing link"):
            m2 = re.search(r"age=(\d+) ms", body)
            if "delivered" in c.t:
                c.mark("close", t)
                c.link_age = int(m2.group(1)) if m2 else None
                c.done.set()
        elif "the eTRV is paused" in body or "outside the device range" in body:
            c.superseded = "refused by the component"
            c.done.set()

    def delivered(self, c, t):
        if "delivered" in c.t:
            return
        c.mark("delivered", t)
        # normally finished by the "closing link" line; never wait forever for it (log lines can drop)
        asyncio.get_running_loop().call_later(10, c.done.set)

    # ---------------------------------------------------------------- helpers
    async def avoid_automation(self):
        """Home Assistant writes set points at hh:m0:00; do not start an operation around that."""
        if not self.args.avoid_automation:
            return
        while True:
            d = datetime.datetime.now()
            s = (d.minute % 10) * 60 + d.second
            # an operation can take a few minutes at a weak signal: start only with >= 4 min left
            # before the automation's next run and >= 90 s after its last one
            if s < 90 or s > 360:
                await asyncio.sleep(3)
                continue
            return

    def names(self):
        return sorted(self.climates)

    def target(self, name):
        st = self.states.get(name)
        return None if st is None else round(st.target_temperature * 2) / 2

    def moved(self, name, sign):
        return max(6.0, min(28.0, self.baseline[name] + sign * self.args.delta))

    async def send(self, name, value, kind, scenario):
        if not self.connected.is_set():
            await asyncio.wait_for(self.connected.wait(), timeout=300)
        c = Cmd(name, value, kind, scenario)
        prev = self.active.get(name)
        if prev is not None and not prev.done.is_set():
            prev.superseded = prev.superseded or "superseded by the next test command"
            prev.done.set()
        self.active[name] = c
        self.sent.setdefault(name, []).append((value, now()))
        self.sent[name] = self.sent[name][-5:]
        self.cli.climate_command(key=self.climates[name].key, target_temperature=value)
        self.note(f"{scenario}: {kind} {name} -> {value:.1f}")
        return c

    async def wait(self, cmds):
        try:
            await asyncio.wait_for(asyncio.gather(*(c.done.wait() for c in cmds)), timeout=self.args.timeout)
        except asyncio.TimeoutError:
            for c in cmds:
                if not c.done.is_set():
                    c.superseded = c.superseded or "TIMEOUT"
                    c.done.set()
        for c in cmds:
            self.results.append(c)
            d = c.rel("delivered")
            if c.no_link:
                self.note(f"  {c.scenario}: {c.name} {c.kind} {c.value:.1f}: no link needed (device value read <60 s ago)")
            else:
                self.note(f"  {c.scenario}: {c.name} {c.kind} {c.value:.1f}: " + (f"delivered after {d:.1f} s (opens failed {c.failed_opens}, link {c.link_age} ms)" if d is not None and not c.superseded else f"NOT measured: {c.superseded}"))

    # ---------------------------------------------------------------- scenarios
    async def sc_parallel(self):
        for r in range(self.args.repeat):
            for sign in (+1, -1):
                await self.avoid_automation()
                t0 = now()
                kind = "write"
                cmds = [await self.send(n, self.moved(n, sign) if sign > 0 else self.baseline[n], kind, "parallel") for n in self.names()]
                await self.wait(cmds)
                ok = [c.rel("delivered") for c in cmds if c.rel("delivered") is not None and not c.superseded and not c.no_link]
                if len(ok) == len(cmds):
                    self.note(f"parallel round {r + 1}{'+' if sign > 0 else '-'}: ALL {len(cmds)} delivered, total {max(ok):.1f} s")
                    self.results.append(("parallel_total", max(ok)))
                else:
                    self.note(f"parallel round {r + 1}: {len(ok)} of {len(cmds)} measured (others superseded or needed no link) - no total")
                await asyncio.sleep(self.args.gap)

    async def sc_cold(self):
        names = self.names()
        for r in range(self.args.repeat):
            name = names[r % len(names)]
            await self.avoid_automation()
            if self.restart_key is None:
                self.note("cold: no restart button found - skipped")
                return
            if not self.connected.is_set():
                await asyncio.wait_for(self.connected.wait(), timeout=300)
            self.note("cold: rebooting the controller")
            self.expect_drop = True
            self.cli.button_command(self.restart_key)
            await asyncio.sleep(3)
            try:
                await self.cli.disconnect()
            except Exception:  # noqa: BLE001
                pass
            self.connected.clear()
            self.states = {}
            await asyncio.sleep(5)
            await self.connect()
            self.expect_drop = False
            t_up = now()
            while self.target(name) is None and now() - t_up < 20:
                await asyncio.sleep(0.2)
            c = await self.send(name, self.moved(name, +1), "write", "cold")
            await self.wait([c])
            await asyncio.sleep(self.args.gap)
            c = await self.send(name, self.baseline[name], "write", "cold-restore")
            await self.wait([c])

    async def sc_ab(self):
        names = self.names()
        pairs = [(a, b) for a in names for b in names if a != b][: max(1, self.args.repeat * 2)]
        for a, b in pairs:
            await self.avoid_automation()
            c1 = await self.send(a, self.moved(a, +1), "write", "ab: A")
            await self.wait([c1])
            c2 = await self.send(b, self.moved(b, +1), "write", "ab: B after A")
            await self.wait([c2])
            c3 = await self.send(a, self.baseline[a], "write", "ab: A again (hot)")
            await self.wait([c3])
            c4 = await self.send(b, self.baseline[b], "write", "ab: B again (hot)")
            await self.wait([c4])
            await asyncio.sleep(self.args.gap)

    async def sc_same(self):
        for r in range(self.args.repeat):
            for n in self.names():
                await self.avoid_automation()
                c1 = await self.send(n, self.moved(n, +1), "write", "same: 1st")
                await self.wait([c1])
                c2 = await self.send(n, self.baseline[n], "write", "same: 2nd (hot)")
                await self.wait([c2])
                await asyncio.sleep(self.args.gap)

    async def sc_double(self):
        for r in range(self.args.repeat):
            for n in self.names():
                await self.avoid_automation()
                first = await self.send(n, self.moved(n, +1), "write", "double: 1st")
                await asyncio.sleep(0.15)
                second = await self.send(n, self.moved(n, -1), "write", "double: 2nd")
                await self.wait([second])
                wrote_first = any(abs(v - first.value) < 0.01 for v in first.written + second.written)
                self.note(f"  double {n}: first value {'WAS' if wrote_first else 'was not'} written (it may only be if its link had already started)")
                self.results.append(("double_first_written", 1.0 if wrote_first else 0.0))
                c = await self.send(n, self.baseline[n], "write", "double: restore")
                await self.wait([c])
                await asyncio.sleep(self.args.gap)

    async def sc_readonly(self):
        for r in range(self.args.repeat):
            await self.readonly_round(r, "readonly")

    async def sc_txab(self):
        # Bluetooth TX power A/B on a test firmware with the two "BLE TX" buttons. Read-only pokes
        # (they do not move the valve motor) keep every eTRV busy: a poke as soon as its last link is
        # older than FRESH_READ_MS; the level alternates every --txab-minutes, after the pokes in
        # flight have finished. The report splits the connection attempts by level (log marks).
        if len(self.tx_buttons) != 2:
            self.note("txab: the test firmware has no 'BLE TX 9/20 dBm' buttons - skipped")
            return
        for r in range(self.args.repeat):
            level = (9, 20)[r % 2]
            if not self.connected.is_set():
                await asyncio.wait_for(self.connected.wait(), timeout=300)
            self.cli.button_command(self.tx_buttons[level])
            self.note(f"txab: BLE TX power {level} dBm")
            t_end = now() + self.args.txab_minutes * 60
            inflight = {}
            while now() < t_end:
                for n in self.names():
                    c = inflight.get(n)
                    if c is not None and not c.done.is_set():
                        continue
                    if now() - self.last_link_end.get(n, 0.0) > 62 and self.connected.is_set():
                        inflight[n] = await self.send(n, self.target(n), "poke", f"txab {level} dBm")
                await asyncio.sleep(1)
            await self.wait([c for c in inflight.values() if not c.done.is_set()])

    async def wait_stale(self):
        # Every eTRV's last read must be older than FRESH_READ_MS (60 s), so that a re-sent set
        # point really opens a (read-only) link; Home Assistant's own writes also refresh it.
        while True:
            await self.avoid_automation()
            ends = [self.last_link_end.get(n, 0.0) for n in self.names()]
            wait = 65 - (now() - max(ends))
            if wait <= 0:
                return
            await asyncio.sleep(wait)

    async def readonly_round(self, r, scenario):
        await self.wait_stale()
        cmds = [await self.send(n, self.target(n), "poke", scenario) for n in self.names()]
        await self.wait(cmds)
        ok = [c.rel("delivered") for c in cmds if c.rel("delivered") is not None and not c.superseded and not c.no_link]
        if len(ok) == len(cmds):
            self.results.append((f"{scenario.replace(' ', '_')}_total", max(ok)))
            self.note(f"{scenario} round {r + 1}: all {len(cmds)} read, total {max(ok):.1f} s")
        await asyncio.sleep(self.args.gap)

    async def sc_mixed(self):
        names = self.names()
        for r in range(self.args.repeat):
            await self.wait_stale()
            half = names[r % 2::2]
            cmds = []
            for n in names:
                if n in half:
                    cmds.append(await self.send(n, self.moved(n, +1), "write", "mixed"))
                else:
                    cmds.append(await self.send(n, self.target(n), "poke", "mixed"))
            await self.wait(cmds)
            back = [await self.send(n, self.baseline[n], "write", "mixed-restore") for n in half]
            await self.wait(back)
            await asyncio.sleep(self.args.gap)

    # ---------------------------------------------------------------- report
    def report(self):
        self.reported = True
        self.note("=" * 60)
        try:
            self.logf.flush()
            self.note(link_stats(self.args.out, since=self.started))
            for line in attempts_by_tx_level(self.args.out):
                self.note(line)
        except Exception as exc:  # noqa: BLE001
            self.note(f"link statistics failed: {exc!r}")
        groups = {}
        for c in self.results:
            if isinstance(c, tuple):
                groups.setdefault(c[0], []).append(c[1])
                continue
            key = c.scenario
            groups.setdefault(key, []).append(c)
        for key, items in groups.items():
            if key == "double_first_written":
                self.note(f"{key:24s} {int(sum(items))} of {len(items)} first values were written before being superseded")
                continue
            if key.endswith("_total"):
                v = sorted(items)
                self.note(f"{key:24s} n={len(v):3d} median={statistics.median(v):6.1f} s max={v[-1]:6.1f} s")
                continue
            ok = [c for c in items if not c.superseded and not c.no_link and c.rel("delivered") is not None]
            if not ok:
                self.note(f"{key:24s} n=0 measured ({len(items)} superseded/timeouts)")
                continue
            def med(vals):
                vals = [v for v in vals if v is not None]
                return f"{statistics.median(vals):5.2f}" if vals else "   - "
            d = sorted(c.rel("delivered") for c in ok)
            self.note(f"{key:24s} n={len(ok):3d} delivered median={statistics.median(d):6.1f} s max={d[-1]:6.1f} s | "
                      f"phases (median, s): link requested {med([c.rel('req') for c in ok])}, open {med([c.rel('open') for c in ok])}, "
                      f"pin {med([c.t['pin'] - c.t['open'] if 'pin' in c.t and 'open' in c.t else None for c in ok])} after open, "
                      f"written {med([c.t['write_rsp'] - c.t['open'] if 'write_rsp' in c.t and 'open' in c.t else None for c in ok])} after open, "
                      f"link {med([c.link_age / 1000 if c.link_age else None for c in ok])} | failed opens {sum(c.failed_opens for c in ok)} | "
                      f"not measured {len(items) - len(ok)}")
        timeouts = [c for c in self.results if not isinstance(c, tuple) and c.superseded == "TIMEOUT"]
        gaps = ", ".join(f"{b - a:.0f} s" for a, b in self.api_gaps) or "none"
        if self.wifi_signal:
            v = sorted(self.wifi_signal)
            self.note(f"device WiFi signal: median {statistics.median(v):.0f} dBm, min {v[0]:.0f} dBm ({len(v)} samples); "
                      f"device WiFi disconnects since its boot: {self.wifi_disconnects}")
        self.note(f"unexpected API connection losses: {len(self.api_gaps)} (back after {gaps})")
        self.note(f"timeouts: {len(timeouts)} | anomalies in the device log: {len(self.anomalies)}")
        for a in self.anomalies[:20]:
            self.note(f"  ANOMALY {a}")

    async def restore(self):
        if not self.connected.is_set():
            await self.connect()
        cmds = []
        for n in self.names():
            if self.target(n) is not None and abs(self.target(n) - self.baseline[n]) > 0.01:
                cmds.append(await self.send(n, self.baseline[n], "write", "restore"))
        if cmds:
            await self.wait(cmds)

    async def run(self):
        await self.connect()
        t0 = now()
        while len(self.states) < len(self.climates) and now() - t0 < 30:
            await asyncio.sleep(0.5)
        self.baseline = {n: self.target(n) for n in self.names()}
        self.note(f"baselines: {self.baseline}")
        d = datetime.datetime.now()
        self.started = d.hour * 3600 + d.minute * 60 + d.second
        try:
            for sc in self.args.scenarios.split(","):
                self.note(f"--- scenario {sc} ---")
                await getattr(self, f"sc_{sc.strip()}")()
        finally:
            self.note("restoring baselines")
            try:
                await self.restore()
            except Exception as exc:  # noqa: BLE001
                self.note(f"restore failed: {exc!r}")
            self.report()
            try:
                await self.cli.disconnect()
            except Exception:  # noqa: BLE001
                pass


async def main(args):
    r = Runner(args)
    task = asyncio.create_task(r.run())
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, task.cancel)
    try:
        await task
    except asyncio.CancelledError:
        # run() restores and reports in its finally block; only if it was cancelled before that
        if not r.reported:
            r.note("cancelled - restoring baselines")
            await r.restore()
            r.report()


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="sterownik-grzejnika.local", help="device IP address or mDNS name")
    p.add_argument("--out", required=True)
    p.add_argument("--scenarios", default="parallel,readonly,same,ab,double,mixed,cold")
    p.add_argument("--repeat", type=int, default=3)
    p.add_argument("--delta", type=float, default=0.5)
    p.add_argument("--timeout", type=float, default=600.0)
    p.add_argument("--gap", type=float, default=5.0, help="seconds between operations")
    p.add_argument("--txab-minutes", type=float, default=8.0, help="minutes per TX power level (scenario txab)")
    p.add_argument("--level", type=int, default=6, help="API log level (6 = verbose)")
    p.add_argument("--no-avoid-automation", dest="avoid_automation", action="store_false")
    asyncio.run(main(p.parse_args()))
