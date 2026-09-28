#!/usr/bin/env python3
"""Controlled mode-change test for ONE Danfoss Eco thermostat.

Records the full published state (mode, action, target, current, battery, problems) and the
device-side SETTINGS/ERRORS log lines, then switches the mode (default AUTO -> HEAT -> AUTO), waiting
for each change to be confirmed by the published state, and prints a before/after comparison so a
"valve reset" (changed min/max/frost/vacation fields, E10 invalid time, target/schedule change) is
visible. It never changes the target temperature.

Usage: mode_test.py --host sterownik-grzejnika.local --thermostat sypialnia_thermostat [--sequence heat,auto]
"""
import argparse
import asyncio
import re
import sys
import time

from aioesphomeapi import (APIClient, BinarySensorInfo, BinarySensorState, ClimateInfo, ClimateMode,
                           ClimateState, LogLevel, SensorInfo, SensorState, TextSensorInfo, TextSensorState)

ANSI = re.compile(r"\x1b\[[0-9;]*m")
MODES = {"heat": ClimateMode.HEAT, "auto": ClimateMode.AUTO, "off": ClimateMode.OFF}


def ts():
    return time.strftime("%H:%M:%S")


async def main(args):
    cli = APIClient(args.host, 6053, None)
    await cli.connect(login=True)
    ents, _ = await cli.list_entities_services()
    climate = next(e for e in ents if isinstance(e, ClimateInfo) and e.object_id == args.thermostat)
    related = {e.key: e for e in ents if isinstance(e, (SensorInfo, BinarySensorInfo, TextSensorInfo)) and args.thermostat.split("_")[0] in e.object_id}
    print(f"{ts()} climate key={climate.key} modes={climate.supported_modes} related={[e.object_id for e in related.values()]}")
    snap = {}
    device_lines = []

    def on_state(st):
        if isinstance(st, ClimateState) and st.key == climate.key:
            snap["climate"] = st
            print(f"{ts()} STATE mode={st.mode} action={st.action} target={st.target_temperature:.1f} current={st.current_temperature:.1f}")
        elif st.key in related and isinstance(st, (SensorState, BinarySensorState, TextSensorState)):
            snap[related[st.key].object_id] = getattr(st, "state", None)

    def on_log(msg):
        t = msg.message.decode("utf-8", "replace") if isinstance(msg.message, (bytes, bytearray)) else msg.message
        t = ANSI.sub("", t)
        if f"[{args.thermostat}]" in t and any(k in t for k in ("SETTINGS", "errors:", "TEMP PROCESSED", "writing mode", "clock", "battery level", "write rsp", "write_request", "mode change", "mode unchanged", "device already", "closing link", "giving up", "retry in", "WATCHDOG", "rejected", "forgetting", "temperature_min", "temperature_max", "frost", "vacation", "adaptable", "lock_control", "valve_installed", "schedule_mode")):
            line = f"{ts()} {t.strip()}"
            device_lines.append(line)
            print("  LOG " + line[:170])

    cli.subscribe_states(on_state)
    cli.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_VERBOSE, dump_config=False)

    # The climate state is published optimistically; the mode is confirmed by the device when the
    # component re-reads the settings block after the write (log: SETTINGS PROCESSED: mode=...).
    # raw modes as logged by the component (see SettingsData::mode_str); OFF = the eTRV "pause"
    RAW_FOR = {ClimateMode.HEAT: ("MANUAL",), ClimateMode.AUTO: ("SCHEDULED", "VACATION"), ClimateMode.OFF: ("PAUSE",)}

    async def wait_mode(mode, timeout, since):
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            for l in device_lines[since:]:
                if "SETTINGS PROCESSED: mode=" in l and any(f"mode={r}" in l for r in RAW_FOR[mode]):
                    return True
                if "mode unchanged" in l or "device already in the requested mode" in l:
                    return True  # the device already had it: nothing to write
            await asyncio.sleep(1)
        return False

    async def refresh_and_snapshot(label, wait):
        await asyncio.sleep(wait)
        st = snap.get("climate")
        rec = {"label": label, "mode": st.mode if st else None, "action": st.action if st else None, "target": st.target_temperature if st else None, "current": st.current_temperature if st else None}
        for k, v in snap.items():
            if k != "climate":
                rec[k] = v
        return rec

    print(f"{ts()} waiting {args.settle}s for the initial state (a poll may be triggered by the component)")
    before = await refresh_and_snapshot("before", args.settle)
    print(f"{ts()} BEFORE: {before}")
    snapshots = [before]
    ok = True
    for step in args.sequence.split(","):
        mode = MODES[step.strip().lower()]
        print(f"{ts()} >>> set mode {step} ({mode})")
        since = len(device_lines)
        cli.climate_command(key=climate.key, mode=mode)
        if not await wait_mode(mode, args.timeout, since):
            print(f"{ts()} !!! mode {step} NOT confirmed within {args.timeout}s")
            ok = False
        rec = await refresh_and_snapshot(f"after {step}", args.after_wait)
        print(f"{ts()} AFTER {step}: {rec}")
        snapshots.append(rec)
    print("=== summary ===")
    keys = sorted({k for s in snapshots for k in s.keys()} - {"label"})
    for k in keys:
        vals = [s.get(k) for s in snapshots]
        flag = " <-- CHANGED" if len({str(v) for v in vals}) > 1 else ""
        print(f"  {k}: {vals}{flag}")
    print("=== device log lines ===")
    for l in device_lines:
        print("  " + l[:200])
    await cli.disconnect()
    return 0 if ok else 1


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="sterownik-grzejnika.local", help="device IP address or mDNS name")
    p.add_argument("--thermostat", required=True)
    p.add_argument("--sequence", default="heat,auto")
    p.add_argument("--settle", type=float, default=20.0)
    p.add_argument("--after-wait", type=float, default=90.0, help="seconds to wait after a confirmed mode change before snapshotting (lets the re-read complete)")
    p.add_argument("--timeout", type=float, default=600.0)
    sys.exit(asyncio.run(main(p.parse_args())))
