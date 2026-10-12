#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check an app's event log for the lifecycle steps of LG's App Self Checklist.

    tools/check-lifecycle-log.py EVENTS_LOG [--expect-relaunch] [--expect-background]
                                 [--max-start-ms 3000] [--max-resume-ms 1000]

The log is what an app on the kit's shell writes to /tmp/<app id>/events.log (tools/logs.sh pulls
it). tools/lifecycle-check.sh drives a TV through a cold start, a relaunch with parameters, going to
the background and back, and a close, then runs this. Each check prints PASS or FAIL with what was
found; the exit code is 1 if any check failed. Stdlib only.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

LINE = re.compile(r"^\s*(\d+)\s+(\w+)\s+\[([^\]]+)\]\s+\(\d+\)\s?(.*)$")


@dataclass
class Event:
    ms: int
    level: str
    category: str
    text: str


def parse(text: str) -> list[Event]:
    events = []
    for line in text.splitlines():
        m = LINE.match(line)
        if m:
            events.append(Event(int(m.group(1)), m.group(2), m.group(3), m.group(4)))
    return events


def first(events: list[Event], needle: str, after: int = 0) -> int | None:
    """Index of the first event at or after `after` whose text contains `needle`."""
    for i in range(after, len(events)):
        if needle in events[i].text:
            return i
    return None


def check(events: list[Event], expect_relaunch: bool, expect_background: bool, max_start_ms: int,
          max_resume_ms: int) -> list[tuple[bool, str, str]]:
    results: list[tuple[bool, str, str]] = []

    def add(ok: bool, name: str, detail: str) -> None:
        results.append((ok, name, detail))

    starts = [e for e in events if e.category == "app" and e.text.startswith("start app_id=")]
    add(len(starts) == 1, "one process for the whole run",
        f"{len(starts)} start line(s)" + ("; the app restarted (crash or kill?)" if len(starts) > 1 else ""))
    crashed = [e for e in events if "previous run crashed" in e.text]
    add(not crashed, "no crash record from an earlier run", crashed[0].text if crashed else "none")

    ff = first(events, "first frame:")
    if ff is None:
        add(False, "first frame drawn", "no 'first frame' line")
        return results
    add(events[ff].ms <= max_start_ms, f"first frame within {max_start_ms} ms of start", f"{events[ff].ms} ms")
    pos = ff

    if expect_relaunch:
        r = first(events, "relaunch: message fields", pos)
        add(r is not None, "relaunch parameters delivered",
            events[r].text if r is not None else "no relaunch line (SDL build without the kit's patch?)")
        pos = r if r is not None else pos

    if expect_background:
        bg = first(events, "-> Suspended", pos)
        add(bg is not None, "went to the background", events[bg].text if bg is not None else "never Suspended")
        if bg is not None:
            res = first(events, "Suspended -> Resuming", bg)
            run = first(events, "Resuming -> Running", res) if res is not None else None
            add(run is not None, "came back to the foreground and drew",
                events[run].text if run is not None else "never Running again")
            if res is not None and run is not None:
                took = events[run].ms - events[res].ms
                add(took <= max_resume_ms, f"first frame after resuming within {max_resume_ms} ms", f"{took} ms")
                pos = run

    done = first(events, "shutdown complete in", pos)
    add(done is not None, "closed through the shutdown sequence",
        events[done].text if done is not None else "no 'shutdown complete' (killed, or the app still runs)")
    watchdog = first(events, "shutdown watchdog")
    add(watchdog is None, "shutdown finished before its watchdog", events[watchdog].text if watchdog is not None else "yes")
    errors = [e for e in events if e.level == "ERROR"]
    add(not errors, "no ERROR lines", f"{len(errors)}: {errors[0].text}" if errors else "none")
    return results


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=Path)
    ap.add_argument("--expect-relaunch", action="store_true")
    ap.add_argument("--expect-background", action="store_true")
    ap.add_argument("--max-start-ms", type=int, default=3000)
    ap.add_argument("--max-resume-ms", type=int, default=1000)
    args = ap.parse_args(argv)
    events = parse(args.log.read_text(errors="replace"))
    if not events:
        print(f"lifecycle: FAIL no log lines in {args.log}")
        return 1
    results = check(events, args.expect_relaunch, args.expect_background, args.max_start_ms, args.max_resume_ms)
    for ok, name, detail in results:
        print(f"lifecycle: {'PASS' if ok else 'FAIL'} {name}: {detail}")
    failed = sum(1 for ok, _, _ in results if not ok)
    print(f"lifecycle: {len(results) - failed}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
