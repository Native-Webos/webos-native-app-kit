# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests for cmake/ExtraApps.cmake (WNK_EXTRA_APP_DIRS): configures a minimal project that
includes the module, with application directories created in a temporary directory.

Run: python3 -m unittest discover -s tests/tools -v   (needs cmake on PATH)
"""
from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

MODULE = Path(__file__).resolve().parents[2] / "cmake" / "ExtraApps.cmake"


@unittest.skipUnless(shutil.which("cmake"), "needs cmake")
class ExtraAppDirsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wnk-extra-"))
        self.src = self.tmp / "project"
        self.src.mkdir()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def app(self, parent: str, name: str) -> Path:
        d = self.tmp / parent / name
        d.mkdir(parents=True)
        (d / "CMakeLists.txt").write_text(
            f'message(STATUS "app {name} at ${{CMAKE_CURRENT_BINARY_DIR}}")\n'
            f'add_custom_target({name}-built)\n'
            "enable_testing()\n"
            f'add_test(NAME {name}.runs COMMAND "${{CMAKE_COMMAND}}" -E true)\n')
        return d

    def configure(self, dirs: list[str]) -> subprocess.CompletedProcess:
        (self.src / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.22)\n"
            "project(extra_apps_test NONE)\n"
            "enable_testing()\n"
            f'include("{MODULE.as_posix()}")\n'
            "wnk_add_extra_app_dirs(${DIRS})\n")
        return subprocess.run(["cmake", "-S", str(self.src), "-B", str(self.tmp / "build"),
                               "-DDIRS=" + ";".join(dirs)],
                              capture_output=True, text=True, check=False)

    def test_each_directory_is_added_under_extra_apps(self):
        a = self.app("one", "alpha")
        b = self.app("two", "beta")
        r = self.configure([str(a), str(b) + "/"])  # a trailing slash is the same directory
        self.assertEqual(r.returncode, 0, r.stderr)
        build = (self.tmp / "build").resolve()
        self.assertIn(f"app alpha at {build}/extra-apps/alpha", r.stdout)
        self.assertIn(f"app beta at {build}/extra-apps/beta", r.stdout)
        tests = subprocess.run(["ctest", "--test-dir", str(build), "-N"], capture_output=True, text=True, check=False)
        self.assertIn("alpha.runs", tests.stdout)
        self.assertIn("beta.runs", tests.stdout)

    def test_no_directories_adds_nothing(self):
        r = self.configure([])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("app ", r.stdout)

    def test_relative_missing_and_duplicate_directories_fail(self):
        a = self.app("one", "alpha")
        same_name = self.app("two", "alpha")
        cases = {
            "is not an absolute path": ["one/alpha"],
            "has no CMakeLists.txt": [str(self.tmp / "nowhere")],
            "two directories are named 'alpha'": [str(a), str(same_name)],
        }
        for message, dirs in cases.items():
            shutil.rmtree(self.tmp / "build", ignore_errors=True)
            r = self.configure(dirs)
            self.assertNotEqual(r.returncode, 0, message)
            self.assertIn(message, r.stderr)


if __name__ == "__main__":
    unittest.main()
