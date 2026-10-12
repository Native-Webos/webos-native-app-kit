# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/check-lifecycle-log.py and tools/soak-report.py on event logs written like the shell's."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name: str, file: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / file)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod  # dataclasses look their module up
    spec.loader.exec_module(mod)
    return mod


life = load("check_lifecycle_log", "check-lifecycle-log.py")
soak = load("soak_report", "soak-report.py")


def line(ms: int, cat: str, text: str, level: str = "INFO") -> str:
    return f"{ms:>10} {level:<5} [{cat}] (1234) {text}"


GOOD = "\n".join([
    line(0, "app", "start app_id=org.x platform=webos params={}"),
    line(80, "lifecycle", "Initialising -> Running (Started)"),
    line(950, "app", "first frame: drawable 1920x1080"),
    line(4000, "lifecycle", "relaunch: message fields [event,parameters], parameters [wnkCheck]"),
    line(7000, "lifecycle", "Running -> Suspended (Background)"),
    line(11000, "lifecycle", "Suspended -> Resuming (Foreground)"),
    line(11200, "lifecycle", "Resuming -> Running (FramePresented)"),
    line(15000, "app", "shutdown complete in 41.0 ms"),
])


class LifecycleLogTest(unittest.TestCase):
    def results(self, text: str, **kw):
        args = dict(expect_relaunch=True, expect_background=True, max_start_ms=3000, max_resume_ms=1000)
        args.update(kw)
        return life.check(life.parse(text), **args)

    def test_a_good_run_passes_every_check(self):
        res = self.results(GOOD)
        self.assertTrue(all(ok for ok, _, _ in res), res)
        self.assertEqual(len(res), 10)

    def test_restart_slow_start_and_missing_steps_fail(self):
        restarted = GOOD + "\n" + line(16000, "app", "start app_id=org.x platform=webos params={}")
        self.assertIn("one process for the whole run", [n for ok, n, _ in self.results(restarted) if not ok])
        slow = GOOD.replace("   950 INFO  [app] (1234) first frame", "  4500 INFO  [app] (1234) first frame")
        failed = [n for ok, n, _ in self.results(slow) if not ok]
        self.assertTrue(any("first frame within" in n for n in failed), failed)
        no_relaunch = "\n".join(l for l in GOOD.splitlines() if "relaunch" not in l)
        self.assertIn("relaunch parameters delivered", [n for ok, n, _ in self.results(no_relaunch) if not ok])
        killed = "\n".join(l for l in GOOD.splitlines() if "shutdown complete" not in l)
        self.assertIn("closed through the shutdown sequence", [n for ok, n, _ in self.results(killed) if not ok])

    def test_slow_resume_errors_and_crash_records_fail(self):
        slow = GOOD.replace("11200 INFO  [lifecycle] (1234) Resuming", "13000 INFO  [lifecycle] (1234) Resuming")
        self.assertTrue(any("after resuming" in n for ok, n, _ in self.results(slow) if not ok))
        err = GOOD + "\n" + line(12000, "net", "boom", level="ERROR")
        self.assertIn("no ERROR lines", [n for ok, n, _ in self.results(err) if not ok])
        crash = line(1, "crash", "previous run crashed: signal=11") + "\n" + GOOD
        self.assertIn("no crash record from an earlier run", [n for ok, n, _ in self.results(crash) if not ok])

    def test_relaunch_and_background_checks_are_optional(self):
        plain = "\n".join(l for l in GOOD.splitlines() if "relaunch" not in l and "Suspended" not in l and "Resuming" not in l)
        res = self.results(plain, expect_relaunch=False, expect_background=False)
        self.assertTrue(all(ok for ok, _, _ in res), res)


def heartbeat(minute: int, rss: float, state: str = "Running") -> str:
    return line(minute * 60000, "heartbeat",
                f"uptime {minute * 60} s, {state}, rss {rss:.1f} MB, presented {minute * 30}, drawn {minute * 600}, views 200")


class SoakReportTest(unittest.TestCase):
    def test_steady_memory_passes_after_warm_up(self):
        lines = [line(0, "app", "start app_id=org.x platform=webos params={}")]
        lines += [heartbeat(m, 60.0 + (20.0 if m >= 3 else 0) + (m % 2) * 0.2) for m in range(1, 241)]  # a cache fills early
        lines.append(line(1000, "perf", "frames 300 presented of 400 drawn: interval avg 16.7 median 16.7 p95 17 max 30 ms, late 2; ..."))
        out, ok = soak.report("\n".join(lines), max_growth=5, warmup_min=10)
        self.assertTrue(ok, out)
        self.assertIn("ran 4.00 h", out[0])
        self.assertTrue(any("2 late in 1 perf windows" in l for l in out), out)

    def test_a_leak_fails(self):
        lines = [line(0, "app", "start app_id=org.x platform=webos params={}")]
        lines += [heartbeat(m, 60.0 + m * 0.5) for m in range(1, 121)]  # 30 MB/h
        out, ok = soak.report("\n".join(lines), max_growth=5, warmup_min=10)
        self.assertFalse(ok)
        self.assertTrue(any("FAIL memory growth" in l and "+30.00 MB/h" in l for l in out), out)

    def test_restarts_crashes_and_missing_heartbeats_fail(self):
        start = line(0, "app", "start app_id=org.x platform=webos params={}")
        beats = [heartbeat(m, 60.0) for m in range(1, 30)]
        out, ok = soak.report("\n".join([start] + beats + [start]), 5, 10)
        self.assertFalse(ok)
        out, ok = soak.report("\n".join([start, line(5, "crash", "previous run crashed: signal=6")] + beats), 5, 10)
        self.assertFalse(ok)
        out, ok = soak.report(start, 5, 10)
        self.assertFalse(ok)
        self.assertIn("no heartbeat", out[0])


if __name__ == "__main__":
    unittest.main()
