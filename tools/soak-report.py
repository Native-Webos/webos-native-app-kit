#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise a long run of a kit app from its event log: memory over time, frame timing, crashes.

    tools/soak-report.py EVENTS_LOG [--max-growth-mb-per-hour 5] [--warmup-min 10]

Reads the shell's once-a-minute heartbeat lines (rss, presented and drawn frames) and its perf lines
(late frames per 300 presented). Memory growth is fitted (least squares) over the heartbeats after the
warm-up, so one cache filling early does not count as a leak. Exit code 1 when the growth exceeds the
limit, the app restarted, or a crash record appeared. tools/soak.sh runs an app on a TV for hours and
calls this; stdlib only.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

LINE = re.compile(r"^\s*(\d+)\s+(\w+)\s+\[([^\]]+)\]\s+\(\d+\)\s?(.*)$")
HEARTBEAT = re.compile(r"uptime (\d+) s, (\w+), rss ([\d.]+) MB, presented (\d+), drawn (\d+)")
PERF_LATE = re.compile(r"late (\d+)")


def slope_per_hour(points: list[tuple[float, float]]) -> float:
    """Least-squares slope of (seconds, MB) points, in MB per hour."""
    n = len(points)
    if n < 2:
        return 0.0
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    den = sum((p[0] - mx) ** 2 for p in points)
    if den == 0:
        return 0.0
    return sum((p[0] - mx) * (p[1] - my) for p in points) / den * 3600.0


def report(text: str, max_growth: float, warmup_min: float) -> tuple[list[str], bool]:
    beats: list[tuple[float, float, str, int, int]] = []
    late = 0
    perf_lines = 0
    starts = 0
    crashes = []
    errors = 0
    for line in text.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        level, cat, msg = m.group(2), m.group(3), m.group(4)
        if level == "ERROR":
            errors += 1
        if cat == "app" and msg.startswith("start app_id="):
            starts += 1
        if "previous run crashed" in msg:
            crashes.append(msg)
        if cat == "heartbeat":
            h = HEARTBEAT.search(msg)
            if h:
                beats.append((float(h.group(1)), float(h.group(3)), h.group(2), int(h.group(4)), int(h.group(5))))
        if cat == "perf":
            perf_lines += 1
            p = PERF_LATE.search(msg)
            if p:
                late += int(p.group(1))
    out: list[str] = []
    ok = True
    if not beats:
        return ["soak: FAIL no heartbeat lines (an app on the kit's shell logs one a minute)"], False
    hours = beats[-1][0] / 3600.0
    out.append(f"soak: ran {hours:.2f} h ({len(beats)} heartbeats), last state {beats[-1][2]}")
    rss = [b[1] for b in beats]
    out.append(f"soak: rss first {rss[0]:.1f} MB, max {max(rss):.1f} MB, last {rss[-1]:.1f} MB")
    settled = [(b[0], b[1]) for b in beats if b[0] >= warmup_min * 60]
    growth = slope_per_hour(settled)
    grew = len(settled) >= 3 and growth > max_growth
    ok &= not grew
    out.append(f"soak: {'FAIL' if grew else 'PASS'} memory growth after {warmup_min:g} min: {growth:+.2f} MB/h "
               f"(limit {max_growth:g}, {len(settled)} points)")
    presented, drawn = beats[-1][3], beats[-1][4]
    out.append(f"soak: frames presented {presented}, drawn {drawn}; {late} late in {perf_lines} perf windows")
    if starts != 1:
        ok = False
        out.append(f"soak: FAIL {starts} process starts in the log (expected 1)")
    if crashes:
        ok = False
        out.append(f"soak: FAIL crash record: {crashes[0]}")
    out.append(f"soak: {errors} ERROR line(s)")
    out.append(f"soak: {'PASS' if ok else 'FAIL'}")
    return out, ok


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=Path)
    ap.add_argument("--max-growth-mb-per-hour", type=float, default=5.0)
    ap.add_argument("--warmup-min", type=float, default=10.0)
    args = ap.parse_args(argv)
    lines, ok = report(args.log.read_text(errors="replace"), args.max_growth_mb_per_hour, args.warmup_min)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
