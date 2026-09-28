#!/usr/bin/env python3
"""Summarize Danfoss Eco BLE links from a phase-2 (transaction model) log."""
import re
import sys
from collections import defaultdict

LINE = re.compile(r"^\[(?P<ts>[^\]]+)\]\[(?P<lvl>[A-Z])\]\[(?P<tag>[^\]]+)\]: (?P<msg>.*)$")
DEV = re.compile(r"^\[(?P<dev>[a-z_]+_thermostat)\] (?P<rest>.*)$")


def main(path, since=None):
    links = defaultdict(list)
    current = {}
    events = defaultdict(list)   # dev -> non-link events (poll, control, retry, give up)
    warnings = []
    tracker = []
    last_ts = None
    for raw in open(path, errors="replace"):
        m = LINE.match(raw.rstrip("\n"))
        if not m:
            continue
        ts, lvl, tag, msg = m.group("ts"), m.group("lvl"), m.group("tag"), m.group("msg")
        if since and ts < since:
            continue
        last_ts = ts
        tagname = tag.split(":")[0]
        if tagname == "esp32_ble_tracker" and any(k in msg for k in ("Stopping scan", "Promoting", "coexistence", "connecting:", "did not")):
            tracker.append((ts, msg))
        if tagname in ("esp32_ble_client", "esp32_ble_tracker", "esp32_ble", "wifi", "api") and lvl in "WE":
            warnings.append((ts, tag, msg))
        if tagname != "danfoss_eco":
            continue
        d = DEV.match(msg)
        if not d:
            if lvl in "WE":
                warnings.append((ts, tag, msg))
            continue
        dev, rest = d.group("dev"), d.group("rest")
        if lvl in "WE" and "LINK status" not in rest:
            warnings.append((ts, tag, f"[{dev}] {rest}"))
        if rest.startswith("connect, conn_id="):
            cur = {"start": ts, "reads": 0, "read_err": 0, "writes": 0, "write_err": 0, "notes": []}
            current[dev] = cur
            links[dev].append(cur)
            continue
        cur = current.get(dev)
        if any(rest.startswith(k) for k in ("poll:", "target temperature change", "mode change", "requesting BLE link", "giving up", "boot:", "tracker did not", "combined", "writing ", "setting the eTRV clock", "not reachable", "reachable again", "new request", "dropping the requested", "device already", "rejecting", "CLIENT WATCHDOG")) or "retry in" in rest or "protocol error" in rest or "wrong secret_key" in rest:
            events[dev].append((ts, rest))
        if cur is None:
            continue
        if rest.startswith("service discovery complete"):
            mm = re.search(r"\((\d+) ms", rest)
            cur["disc_ms"] = int(mm.group(1)) if mm else None
        elif rest.startswith("pin OK"):
            mm = re.search(r"\((\d+) ms", rest)
            cur["pin_ms"] = int(mm.group(1)) if mm else None
        elif rest.startswith("pin FAILED"):
            cur["notes"].append("PIN-FAILED")
        elif rest.startswith("read rsp"):
            cur["reads" if "status=0000" in rest else "read_err"] += 1
        elif rest.startswith("write rsp"):
            cur["writes" if "status=0000" in rest else "write_err"] += 1
        elif rest.startswith("closing link"):
            mm = re.search(r"closing link \(([^)]*)\): age=(\d+)", rest)
            cur["closed_by_us"] = mm.group(1) if mm else rest
            cur["done_ms"] = int(mm.group(2)) if mm else None
        elif rest.startswith("close, conn_id=") or rest.startswith("disconnect, conn_id="):
            mm = re.search(r"reason=(0x[0-9a-f]+|[0-9]+).*link_age=(\d+)", rest)
            kind = "CLOSE" if rest.startswith("close") else "DISC"
            cur.setdefault("end", []).append(f"{kind}:{mm.group(1) if mm else '?'}")
            cur["end_ts"] = ts
            if kind == "CLOSE":
                current.pop(dev, None)
        elif "WATCHDOG" in rest or "link lost" in rest or "request timeout" in rest or "unexpected" in rest:
            cur["notes"].append(rest[:90])

    print(f"log until {last_ts}")
    for dev in sorted(set(list(links) + list(events))):
        ls = links.get(dev, [])
        print(f"\n=== {dev}: {len(ls)} links ===")
        merged = [(l["start"], "L", l) for l in ls] + [(ts, "E", rest) for ts, rest in events.get(dev, [])]
        merged.sort(key=lambda x: x[0])
        for ts, kind, x in merged:
            if kind == "E":
                print(f"    {ts[11:]} {x}")
            else:
                l = x
                flags = " ".join(l["notes"])
                if "end" not in l:
                    flags += " STILL-OPEN?"
                print(f"  {ts[11:]} disc={l.get('disc_ms')}ms pin={l.get('pin_ms')}ms r={l['reads']}/{l['read_err']} w={l['writes']}/{l['write_err']} closed_by_us={l.get('closed_by_us','NO')} at {l.get('done_ms')}ms end={','.join(l.get('end', []))} {flags}")
    print("\n=== warnings/errors ===")
    for ts, tag, msg in warnings:
        print(f"  {ts[11:]} [{tag}] {msg}")
    print("\n=== tracker ===")
    for ts, msg in tracker[-40:]:
        print(f"  {ts[11:]} {msg}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
