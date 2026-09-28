#!/usr/bin/env python3
"""Long-running BLE lifecycle test for the danfoss_eco ESPHome component.

Captures the device log over the native API and exercises the thermostats:
  * every --period seconds, one *round*: the target temperature of every thermostat (parallel) or of
    one thermostat (--single) is moved by +delta, the next round moves it back (-delta), so the
    thermostats alternate between T0 and T0+delta and always end at T0,
  * every command is VERIFIED: the published climate state must report the commanded target within
    --verify-timeout seconds, otherwise it is counted as a control failure,
  * optionally the device is rebooted every --restart-every rounds (restart button entity),
  * external setpoint changes (e.g. a Home Assistant automation) are detected from the state stream
    and simply become the new baseline of that thermostat.

Writes <prefix>.log (device log, host timestamps, ANSI stripped), <prefix>.state (climate states),
<prefix>.cmd (commands, verifications, summary). Ctrl-C / SIGTERM restores all baselines and exits.
"""
import argparse
import asyncio
import datetime
import math
import re
import signal
import sys
import time

from aioesphomeapi import APIClient, ButtonInfo, ClimateInfo, ClimateState, LogLevel

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def ts() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


class Runner:
    def __init__(self, args):
        self.args = args
        self.logf = open(args.prefix + ".log", "a", buffering=1)
        self.statef = open(args.prefix + ".state", "a", buffering=1)
        self.cmdf = open(args.prefix + ".cmd", "a", buffering=1)
        self.climates: dict[int, ClimateInfo] = {}
        self.restart_key: int | None = None
        self.states: dict[int, ClimateState] = {}
        self.baseline: dict[int, float] = {}      # T0 per thermostat
        self.expected: dict[int, float] = {}      # target we last commanded (None = none pending)
        self.cli: APIClient | None = None
        self.stop = asyncio.Event()
        self.stats = {"sent": 0, "verified": 0, "failed": 0, "superseded": 0, "max_latency": 0.0, "restarts": 0, "reconnects": 0, "external": 0}
        self.recent: dict[int, list[float]] = {}                  # key -> last few targets this script sent
        self.latencies: list[float] = []
        self.sent_log: dict[int, list[tuple[float, float]]] = {}
        self.pending: dict[int, tuple[float, float]] = {}   # key -> (target, sent_at)
        self.last_cmd: dict[int, float] = {}                 # key -> last target this script sent

    def note(self, f, text):
        line = f"{ts()} {text}"
        f.write(line + "\n")
        print(line, flush=True)

    # ---------------- API plumbing ----------------
    async def connect(self):
        self.cli = APIClient(self.args.host, self.args.port, None)
        await self.cli.connect(login=True, on_stop=self.on_stop)
        info = await self.cli.device_info()
        self.note(self.cmdf, f"connected: {info.name} esphome={info.esphome_version} compiled={info.compilation_time}")
        entities, _ = await self.cli.list_entities_services()
        for e in entities:
            if isinstance(e, ClimateInfo):
                self.climates[e.key] = e
            elif isinstance(e, ButtonInfo) and "restart" in e.object_id.lower() and "safe" not in e.object_id.lower():
                self.restart_key = e.key
        self.cli.subscribe_logs(self.on_log, log_level=LogLevel(self.args.log_level), dump_config=False)
        self.cli.subscribe_states(self.on_state)

    async def on_stop(self, expected_disconnect: bool):
        self.note(self.cmdf, f"API connection lost (expected={expected_disconnect})")
        self.cli = None

    TEMP_RE = re.compile(r"\[(\w+)\] TEMP PROCESSED: room=[\d.]+ target=([\d.]+)")
    CHANGE_RE = re.compile(r"\[(\w+)\] target temperature change: \S+ -> ([\d.]+) C")

    def on_log(self, msg):
        text = msg.message
        if isinstance(text, (bytes, bytearray)):
            text = text.decode("utf-8", "replace")
        text = ANSI.sub('', text)
        self.logf.write(f"[{ts()}]{text}\n")
        # A set point request that this script did not send in the last 3 s (a Home Assistant
        # automation) replaces ours: the component delivers the newest request, so stop waiting.
        m = self.CHANGE_RE.search(text)
        if m:
            key = next((k for k, c in self.climates.items() if c.object_id == m.group(1)), None)
            pend = self.pending.get(key) if key is not None else None
            value = float(m.group(2))
            if pend is not None and abs(value - pend[0]) >= 0.05:
                sent = self.sent_log.get(key, [])
                mine = any(abs(v - value) < 0.05 and time.monotonic() - t < 3.0 for t, v in sent)
                if not mine:
                    self.stats["superseded"] += 1
                    self.note(self.cmdf, f"SUPERSEDED {m.group(1)} target={pend[0]:.1f} by another client's {value:.1f} (device log) -> new baseline")
                    self.pending.pop(key, None)
                    self.baseline[key] = value
        # Delivery is verified by the DEVICE: the component re-reads the temperature characteristic
        # after a write and logs what the eTRV reports (the climate state is published optimistically).
        m = self.TEMP_RE.search(text)
        if m:
            key = next((k for k, c in self.climates.items() if c.object_id == m.group(1)), None)
            pend = self.pending.get(key) if key is not None else None
            if pend is not None and abs(float(m.group(2)) - pend[0]) < 0.05:
                latency = time.monotonic() - pend[1]
                self.stats["verified"] += 1
                self.stats["max_latency"] = max(self.stats["max_latency"], latency)
                self.latencies.append(latency)
                self.note(self.cmdf, f"VERIFIED {m.group(1)} target={pend[0]:.1f} read back from the device after {latency:.1f}s")
                self.pending.pop(key, None)

    def on_state(self, st):
        if not isinstance(st, ClimateState):
            return
        self.states[st.key] = st
        name = self.climates[st.key].object_id if st.key in self.climates else str(st.key)
        self.note(self.statef, f"{name} mode={st.mode} action={st.action} cur={st.current_temperature:.1f} target={st.target_temperature:.1f}")
        if math.isnan(st.target_temperature):
            return
        if st.key not in self.baseline:
            self.baseline[st.key] = st.target_temperature
            self.note(self.cmdf, f"baseline for {name}: {st.target_temperature:.1f}")
            return
        pend = self.pending.get(st.key)
        if pend is not None:
            target, sent_at = pend
            if abs(st.target_temperature - target) < 0.05:
                pass  # optimistic publish; delivery is verified from the device log (on_log)
            elif all(abs(st.target_temperature - v) >= 0.05 for v in self.recent.get(st.key, [])) and abs(st.target_temperature - self.baseline[st.key]) >= 0.05:
                # a value nobody here ever sent while our command was pending: an external command
                # (Home Assistant automation) superseded ours - the component applies the newest request
                self.stats["superseded"] += 1
                self.note(self.cmdf, f"SUPERSEDED {name} target={target:.1f} by external {st.target_temperature:.1f} -> new baseline")
                self.pending.pop(st.key, None)
                self.baseline[st.key] = st.target_temperature
            return
        # no command pending: a target that is neither the baseline nor something this script sent
        # is external (HA automation) -> new baseline
        low, high = self.baseline[st.key], self.baseline[st.key] + self.args.delta
        mine = st.key in self.last_cmd and abs(st.target_temperature - self.last_cmd[st.key]) < 0.05
        if not mine and abs(st.target_temperature - low) >= 0.05 and abs(st.target_temperature - high) >= 0.05:
            self.stats["external"] += 1
            self.note(self.cmdf, f"EXTERNAL change on {name}: target={st.target_temperature:.1f} -> new baseline")
            self.baseline[st.key] = st.target_temperature

    async def ensure_connected(self):
        while not self.stop.is_set():
            try:
                if self.cli is None:
                    await self.connect()
                    self.stats["reconnects"] += 1
                    return True
                await self.cli.device_info()
                return True
            except Exception as exc:  # noqa: BLE001
                self.note(self.cmdf, f"API connection problem: {exc!r} - reconnecting in 1s")
                try:
                    if self.cli is not None:
                        await self.cli.disconnect()
                except Exception:  # noqa: BLE001
                    pass
                self.cli = None
                await asyncio.sleep(1)
        return False

    def send_target(self, key: int, target: float, why: str):
        name = self.climates[key].object_id
        self.note(self.cmdf, f"CMD {name} target={target:.1f} ({why})")
        self.stats["sent"] += 1
        self.pending[key] = (target, time.monotonic())
        self.last_cmd[key] = target
        self.recent[key] = (self.recent.get(key, []) + [target])[-4:]
        self.sent_log[key] = (self.sent_log.get(key, []) + [(time.monotonic(), target)])[-6:]
        self.cli.climate_command(key=key, target_temperature=target)

    def check_pending_timeouts(self):
        now = time.monotonic()
        for key, (target, sent_at) in list(self.pending.items()):
            if now - sent_at > self.args.verify_timeout:
                self.stats["failed"] += 1
                st = self.states.get(key)
                cur = f"{st.target_temperature:.1f}" if st is not None else "?"
                self.note(self.cmdf, f"FAILED {self.climates[key].object_id} target={target:.1f} not confirmed within {self.args.verify_timeout}s (state shows {cur})")
                self.pending.pop(key, None)

    async def wait(self, seconds: float) -> bool:
        """Wait, keeping the connection alive and checking verification timeouts. True = stop requested."""
        remaining = seconds
        while remaining > 0:
            slice_ = min(2.0, remaining)
            try:
                await asyncio.wait_for(self.stop.wait(), timeout=slice_)
                return True
            except asyncio.TimeoutError:
                remaining -= slice_
            self.check_pending_timeouts()
            if self.cli is None:
                await self.ensure_connected()
        return False

    # ---------------- exercise plan ----------------
    async def run(self):
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, self.stop.set)
        if not await self.ensure_connected():
            return
        self.note(self.cmdf, f"waiting {self.args.settle}s for initial states")
        if await self.wait(self.args.settle):
            return

        keys = list(self.climates.keys())
        round_no = 0
        up = True
        single_idx = 0
        try:
            while not self.stop.is_set() and (self.args.rounds == 0 or round_no < self.args.rounds):
                round_no += 1
                if not await self.ensure_connected():
                    break
                if self.args.restart_every and round_no > 1 and (round_no - 1) % self.args.restart_every == 0 and self.restart_key is not None:
                    # never reboot with commands in flight: a reboot legitimately loses them (RAM state)
                    waited = 0.0
                    while self.pending and waited < self.args.verify_timeout and not self.stop.is_set():
                        if await self.wait(2.0):
                            break
                        waited += 2.0
                    if self.stop.is_set():
                        break
                    self.note(self.cmdf, f"--- round {round_no}: REBOOTING the device (pending={len(self.pending)}) ---")
                    self.stats["restarts"] += 1
                    if not await self.ensure_connected():
                        break
                    self.cli.button_command(key=self.restart_key)
                    if await self.wait(self.args.reboot_settle):
                        break
                    if not await self.ensure_connected():
                        break
                if self.args.single:
                    targets = [keys[single_idx % len(keys)]]
                    single_idx += 1
                else:
                    targets = keys
                targets = [k for k in targets if k in self.baseline and k not in self.pending and self.climates[k].object_id not in self.args.unreachable]
                self.note(self.cmdf, f"--- round {round_no}: {'UP' if up else 'DOWN'} on {len(targets)} thermostat(s) ---")
                for k in targets:
                    target = self.baseline[k] + (self.args.delta if up else 0.0)
                    self.send_target(k, target, f"round {round_no} {'up' if up else 'down'}")
                for name in self.args.unreachable:
                    key = next((k for k, c in self.climates.items() if c.object_id == name), None)
                    if key is not None:
                        # exercised, never expected to be delivered (e.g. a thermostat that does not exist)
                        t = 20.0 + (1.0 if up else 0.0)
                        self.note(self.cmdf, f"CMD-UNREACHABLE {name} target={t:.1f}")
                        self.cli.climate_command(key=key, target_temperature=t)
                if self.args.double_tap and targets:
                    # a second, different value right behind the first (HA slider behaviour): the
                    # LAST value must win
                    if await self.wait(self.args.double_tap):
                        break
                    for k in targets:
                        target = self.baseline[k] + (self.args.delta / 2 if up else self.args.delta)
                        self.send_target(k, target, f"round {round_no} double-tap")
                up = not up
                if await self.wait(self.args.period):
                    break
        finally:
            await self.restore_all()

    async def restore_all(self):
        self.note(self.cmdf, "restoring baselines")
        try:
            if await self.ensure_connected():
                for k, t in self.baseline.items():
                    st = self.states.get(k)
                    if st is None or math.isnan(st.target_temperature) or abs(st.target_temperature - t) >= 0.05:
                        self.send_target(k, t, "final restore")
                if await self.wait(self.args.tail_after_restore):
                    pass
        except Exception as exc:  # noqa: BLE001
            self.note(self.cmdf, f"restore failed: {exc!r}")
        self.check_pending_timeouts()
        s = self.stats
        lat = sorted(self.latencies)
        if lat:
            self.note(self.cmdf, f"LATENCY n={len(lat)} median={lat[len(lat)//2]:.1f}s p90={lat[int(len(lat)*0.9)]:.1f}s max={lat[-1]:.1f}s")
        self.note(self.cmdf, f"SUMMARY sent={s['sent']} verified={s['verified']} failed={s['failed']} superseded_by_external={s['superseded']} still_pending={len(self.pending)} max_latency={s['max_latency']:.1f}s restarts={s['restarts']} api_reconnects={s['reconnects']} external_changes={s['external']}")
        if self.cli is not None:
            try:
                await self.cli.disconnect()
            except Exception:  # noqa: BLE001
                pass
        self.note(self.cmdf, "done")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="sterownik-grzejnika.local", help="device IP address or mDNS name")
    p.add_argument("--port", type=int, default=6053)
    p.add_argument("--prefix", required=True)
    p.add_argument("--period", type=float, default=90.0, help="seconds between rounds")
    p.add_argument("--settle", type=float, default=90.0, help="seconds to wait for initial states")
    p.add_argument("--rounds", type=int, default=0, help="0 = run until stopped")
    p.add_argument("--delta", type=float, default=1.0)
    p.add_argument("--single", action="store_true", help="one thermostat per round instead of all in parallel")
    p.add_argument("--restart-every", type=int, default=0, help="reboot the device every N rounds (0 = never)")
    p.add_argument("--reboot-settle", type=float, default=120.0, help="seconds to wait after a reboot")
    p.add_argument("--verify-timeout", type=float, default=300.0, help="seconds a command may take to be confirmed")
    p.add_argument("--tail-after-restore", type=float, default=90.0)
    p.add_argument("--log-level", type=int, default=7, help="API log subscription level (7 = very verbose)")
    p.add_argument("--double-tap", type=float, default=0.0, help="seconds after each command send a second, different value (0 = off)")
    p.add_argument("--unreachable", action="append", default=[], help="object_id of a thermostat that also gets commands but is not expected to answer (repeatable)")
    args = p.parse_args()
    asyncio.run(Runner(args).run())


if __name__ == "__main__":
    sys.exit(main())
