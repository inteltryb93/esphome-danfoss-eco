#!/usr/bin/env python3
"""Show everything the Danfoss eTRVs expose, optionally after triggering one BLE link per thermostat.

"Poking" sends each thermostat its CURRENT set point: the component opens one link (PIN, state read,
clock sync / E10 acknowledgment if needed, informational reads after boot) and writes nothing to
the set point, because the eTRV already has that value.

Usage: probe.py --host sterownik-grzejnika.local [--poke] [--only kuchnia] [--log run.log] [--wait 240]
"""
import argparse
import asyncio
import datetime
import re

from aioesphomeapi import (APIClient, BinarySensorInfo, ClimateInfo, ClimateState, LogLevel, SensorInfo,
                           TextSensorInfo)

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


async def main(args):
    cli = APIClient(args.host, 6053, None)
    await cli.connect(login=True)
    ents, _ = await cli.list_entities_services()
    climates = {e.key: e for e in ents if isinstance(e, ClimateInfo) and (not args.only or e.object_id.startswith(args.only))}
    info = {e.key: e for e in ents if isinstance(e, (SensorInfo, BinarySensorInfo, TextSensorInfo))}
    states, climate_state = {}, {}
    done = set()
    logf = open(args.log, "a", buffering=1) if args.log else None

    def on_state(st):
        if isinstance(st, ClimateState) and st.key in climates:
            climate_state[st.key] = st
        elif st.key in info:
            states[st.key] = st

    def on_log(msg):
        t = ANSI.sub("", msg.message.decode("utf-8", "replace") if isinstance(msg.message, (bytes, bytearray)) else msg.message)
        if logf:
            logf.write(f"[{ts()}]{t}\n")
        for key, c in climates.items():
            if f"[{c.object_id}] closing link (transaction complete)" in t:
                done.add(key)
        if any(k in t for k in ("setting the eTRV clock", "acknowledging E10", "eTRV reports", "schedule:", "firmware revision", "device name", "PIN protection", "giving up", "WATCHDOG", "writing ")):
            print(f"{ts()} {t.strip()[:200]}")

    cli.subscribe_states(on_state)
    cli.subscribe_logs(on_log, log_level=LogLevel(args.level), dump_config=False)
    await asyncio.sleep(3)
    if args.poke:
        for key, c in climates.items():
            st = climate_state.get(key)
            if st is None or st.target_temperature != st.target_temperature:
                print(f"{ts()} {c.object_id}: no state yet, skipping")
                continue
            print(f"{ts()} poke {c.object_id}: re-sending current set point {st.target_temperature:.1f}")
            cli.climate_command(key=key, target_temperature=st.target_temperature)
        t0 = asyncio.get_running_loop().time()
        while len(done) < len(climates) and asyncio.get_running_loop().time() - t0 < args.wait:
            await asyncio.sleep(1)
        print(f"{ts()} links completed: {sorted(climates[k].object_id for k in done)}")
        await asyncio.sleep(5)
    for key, c in sorted(climates.items(), key=lambda kv: kv[1].object_id):
        room = c.object_id.replace("_thermostat", "")
        st = climate_state.get(key)
        print(f"\n=== {c.object_id}: mode={st.mode.name if st else '?'} action={st.action.name if st and st.action is not None else '?'} target={st.target_temperature if st else '?'} current={st.current_temperature if st else '?'}")
        for k, e in sorted(info.items(), key=lambda kv: kv[1].object_id):
            if room not in e.object_id:
                continue
            s = states.get(k)
            val = "-" if s is None else ("unknown" if getattr(s, "missing_state", False) else s.state)
            print(f"  {e.object_id[len('sterownik_grzejnika_'):] if e.object_id.startswith('sterownik_grzejnika_') else e.object_id:60s} {val}")
    await cli.disconnect()


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="sterownik-grzejnika.local", help="device IP address or mDNS name")
    p.add_argument("--poke", action="store_true")
    p.add_argument("--only", default="")
    p.add_argument("--log", default="")
    p.add_argument("--level", type=int, default=6)
    p.add_argument("--wait", type=float, default=240.0)
    asyncio.run(main(p.parse_args()))
