#!/usr/bin/env python3
"""Long-running health recorder for the heating controller (read-only).

Every --interval seconds it writes one CSV line: time, API connected, free heap, largest free heap
block, uptime (if an `uptime` sensor exists), and per thermostat the target / room temperature and
the `connection` sensor. It reconnects automatically and counts reconnects, so unexpected reboots
and slow memory leaks become visible over hours.

Usage: soak_monitor.py [--host sterownik-grzejnika.local] --out soak.csv [--interval 60] [--duration 0]
"""
import argparse
import asyncio
import csv
import datetime
import signal

from aioesphomeapi import APIClient, BinarySensorInfo, ClimateInfo, ClimateState, SensorInfo


async def main(args):
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    if args.duration > 0:
        loop.call_later(args.duration, stop.set)
    f = open(args.out, "a", newline="", buffering=1)
    w = csv.writer(f)
    reconnects = -1
    header_done = f.tell() > 0
    while not stop.is_set():
        cli = APIClient(args.host, 6053, None)
        lost = asyncio.Event()

        async def on_stop(expected):
            lost.set()

        try:
            await cli.connect(login=True, on_stop=on_stop)
            reconnects += 1
            ents, _ = await cli.list_entities_services()
            climates = {e.key: e.object_id for e in ents if isinstance(e, ClimateInfo)}
            sensors = {e.key: e.object_id for e in ents if isinstance(e, SensorInfo) and any(k in e.object_id for k in ("heap", "uptime"))}
            conns = {e.key: e.object_id for e in ents if isinstance(e, BinarySensorInfo) and e.object_id.endswith("_connection")}
            state = {}

            def on_state(st):
                if isinstance(st, ClimateState) and st.key in climates:
                    state[climates[st.key]] = (st.target_temperature, st.current_temperature)
                elif st.key in sensors or st.key in conns:
                    state[(sensors | conns)[st.key]] = getattr(st, "state", None)

            cli.subscribe_states(on_state)
            names = sorted(climates.values())
            if not header_done:
                w.writerow(["time", "api", "reconnects", "heap_free", "heap_max_block", "uptime"] + [f"{n}_{k}" for n in names for k in ("target", "room", "conn")])
                header_done = True
            while not stop.is_set() and not lost.is_set():
                await asyncio.sleep(args.interval)
                def val(sub):
                    return next((v for k, v in state.items() if isinstance(k, str) and sub in k), "")
                row = [datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), 1, reconnects, val("heap_free"), val("heap_max_block"), val("uptime")]
                for n in names:
                    t, r = state.get(n, ("", ""))
                    c = next((v for k, v in state.items() if isinstance(k, str) and k.endswith(n.replace("_thermostat", "") + "_thermostat_connection")), "")
                    row += [t, r, c]
                w.writerow(row)
        except Exception as exc:  # noqa: BLE001
            w.writerow([datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), 0, reconnects, f"error: {exc!r}"[:80]])
            await asyncio.sleep(10)
        finally:
            try:
                await cli.disconnect()
            except Exception:  # noqa: BLE001
                pass
    f.close()


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="sterownik-grzejnika.local", help="device IP address or mDNS name")
    p.add_argument("--out", required=True)
    p.add_argument("--interval", type=float, default=60.0)
    p.add_argument("--duration", type=float, default=0.0, help="seconds, 0 = until stopped")
    asyncio.run(main(p.parse_args()))
