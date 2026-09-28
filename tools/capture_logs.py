#!/usr/bin/env python3
"""Capture the complete device log (and climate states) over the native API until stopped.

Usage: capture_logs.py --host sterownik-grzejnika.local --out run.log [--level 6] [--duration 0]
"""
import argparse
import asyncio
import datetime
import re
import signal

from aioesphomeapi import APIClient, ClimateInfo, ClimateState, LogLevel

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def ts():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


async def main(args):
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    if args.duration > 0:
        loop.call_later(args.duration, stop.set)
    out = open(args.out, "a", buffering=1)
    while not stop.is_set():
        cli = APIClient(args.host, 6053, None)
        lost = asyncio.Event()

        async def on_stop(expected):
            out.write(f"[{ts()}][capture] API connection lost\n")
            lost.set()

        try:
            await cli.connect(login=True, on_stop=on_stop)
            ents, _ = await cli.list_entities_services()
            names = {e.key: e.object_id for e in ents if isinstance(e, ClimateInfo)}

            def on_log(msg):
                t = msg.message.decode("utf-8", "replace") if isinstance(msg.message, (bytes, bytearray)) else msg.message
                out.write(f"[{ts()}]{ANSI.sub('', t)}\n")

            def on_state(st):
                if isinstance(st, ClimateState) and st.key in names:
                    out.write(f"[{ts()}][capture] STATE {names[st.key]} mode={st.mode.name if st.mode is not None else None} target={st.target_temperature:.1f} current={st.current_temperature:.1f}\n")

            cli.subscribe_logs(on_log, log_level=LogLevel(args.level), dump_config=False)
            cli.subscribe_states(on_state)
            out.write(f"[{ts()}][capture] connected\n")
            done, _ = await asyncio.wait([asyncio.create_task(stop.wait()), asyncio.create_task(lost.wait())], return_when=asyncio.FIRST_COMPLETED)
        except Exception as exc:  # noqa: BLE001
            out.write(f"[{ts()}][capture] connect failed: {exc!r}\n")
            await asyncio.sleep(1)
        finally:
            try:
                await cli.disconnect()
            except Exception:  # noqa: BLE001
                pass


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="sterownik-grzejnika.local", help="device IP address or mDNS name")
    p.add_argument("--out", required=True)
    p.add_argument("--level", type=int, default=6)
    p.add_argument("--duration", type=float, default=0)
    asyncio.run(main(p.parse_args()))
