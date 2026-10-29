# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests for tools/new-app.py: an app made in a temporary folder outside the kit configures
against the kit through cmake/WnkApp.cmake, with its smoke test listed; bad input is refused.

Run: python3 -m unittest discover -s tests/tools -v   (needs cmake on PATH)
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

KIT = Path(__file__).resolve().parents[2]
TOOL = KIT / "tools" / "new-app.py"


def new_app(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, check=False)


class NewAppTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wnk-new-app-"))

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_writes_a_standalone_project(self):
        d = self.tmp / "weather"
        r = new_app(str(d), "org.example.weather", "--title", "Weather Demo", "--vendor", "Example")
        self.assertEqual(r.returncode, 0, r.stderr)
        for f in ("CMakeLists.txt", "CMakePresets.json", "build.sh", "main.cpp", "README.md", ".gitignore",
                  "expected-needed.txt", "icon.png", "largeIcon.png"):
            self.assertTrue((d / f).is_file(), f)
        self.assertTrue(os.access(d / "build.sh", os.X_OK))
        cm = (d / "CMakeLists.txt").read_text()
        self.assertIn("APPID org.example.weather", cm)
        self.assertIn('TITLE "Weather Demo"', cm)
        self.assertIn("cmake/WnkApp.cmake", cm)
        self.assertIn(str(KIT), cm)  # far from the kit: its absolute path
        self.assertIn('config.fallback_app_id = "org.example.weather"', (d / "main.cpp").read_text())
        self.assertEqual((d / "icon.png").read_bytes()[16:24], (80).to_bytes(4, "big") * 2)  # IHDR size
        self.assertEqual((d / "largeIcon.png").read_bytes()[16:24], (130).to_bytes(4, "big") * 2)

    def test_refuses_bad_input(self):
        cases = [
            ([str(self.tmp / "a"), "Org.Example", "--title", "A"], "reverse-DNS"),
            ([str(self.tmp / "a"), "weather", "--title", "A"], "reverse-DNS"),
            ([str(self.tmp / "a"), "com.webos.mine", "--title", "A"], "LG keeps"),
            ([str(self.tmp / "a"), "org.example.a", "--title", 'Say "hi"'], "--title"),
            ([str(self.tmp / "a"), "org.example.a", "--title", "A", "--color", "red"], "--color"),
            ([str(KIT / "applications" / "mine"), "org.example.a", "--title", "A"], "inside the kit"),
        ]
        for args, why in cases:
            r = new_app(*args)
            self.assertEqual(r.returncode, 2, args)
            self.assertIn(why, r.stderr, args)
        self.assertFalse((KIT / "applications" / "mine").exists())
        full = self.tmp / "full"
        full.mkdir()
        (full / "keep.txt").write_text("x")
        r = new_app(str(full), "org.example.a", "--title", "A")
        self.assertEqual(r.returncode, 2)
        self.assertIn("not an empty folder", r.stderr)

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"), "needs cmake")
    def test_configures_against_the_kit_with_its_smoke_test(self):
        d = self.tmp / "hello2"
        self.assertEqual(new_app(str(d), "org.example.hello2", "--title", "Hello Two").returncode, 0)
        env = {k: v for k, v in os.environ.items() if k != "WNK_KIT_DIR"}
        build = self.tmp / "build"
        r = subprocess.run(["cmake", "-S", str(d), "-B", str(build)], capture_output=True, text=True, check=False, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        tests = subprocess.run(["ctest", "--test-dir", str(build), "-N"], capture_output=True, text=True, check=False)
        self.assertIn("e2e.hello2.starts_and_quits", tests.stdout)
        self.assertNotIn("unit.", tests.stdout)  # the kit's own tests stay out of an app's build
        # WNK_KIT_DIR wins over the recorded path; a wrong one says so.
        r = subprocess.run(["cmake", "-S", str(d), "-B", str(self.tmp / "b2"), "-DWNK_KIT_DIR=" + str(self.tmp)],
                           capture_output=True, text=True, check=False, env=env)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("The webOS native app kit is not at", r.stderr)


if __name__ == "__main__":
    unittest.main()
