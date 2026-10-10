# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests for tools/mkipk.py, tools/check-package.py, tools/wnk_elf.py and tools/fw-check.py.

Run: python3 -m unittest discover -s tests/tools -v
Uses a host ELF (the Python interpreter) as a stand-in executable with --arch x86_64, so no NDK is
needed. ARM-specific checks are exercised by the webOS build's <target>-ipk step instead.
"""
from __future__ import annotations

import importlib.util
import io
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))

import mkipk  # noqa: E402
import wnk_elf  # noqa: E402
import wnk_fwdb  # noqa: E402
import wnk_ipk  # noqa: E402


def load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), TOOLS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


check_package = load_tool("check-package")
fw_check = load_tool("fw-check")

HOST_ELF = Path(os.path.realpath(sys.executable))


def make_stage(root: Path, app_id: str = "org.example.demo", version: str = "1.2.3") -> Path:
    stage = root / "stage"
    (stage / "lib").mkdir(parents=True)
    shutil.copyfile(HOST_ELF, stage / "demo")
    (stage / "demo").chmod(0o755)
    (stage / "icon.png").write_bytes(b"\x89PNG\r\n\x1a\nfake")
    (stage / "appinfo.json").write_text(json.dumps({
        "id": app_id, "version": version, "vendor": "Example", "type": "native",
        "main": "demo", "title": "Demo", "icon": "icon.png"}))
    return stage


def build(stage: Path, out: Path) -> Path:
    with redirect_stdout(io.StringIO()):
        return mkipk.build(stage, out, "Example Dev <dev@example.org>", None, "GPL-3.0-or-later", None, "x86_64")


class MkipkTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wnk-pkg-"))

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_reproducible(self):
        stage = make_stage(self.tmp)
        a = build(stage, self.tmp / "a").read_bytes()
        os.utime(stage / "icon.png", (1, 1))  # mtimes must not matter
        b = build(stage, self.tmp / "b").read_bytes()
        self.assertEqual(a, b)

    def test_layout(self):
        ipk = build(make_stage(self.tmp), self.tmp / "out")
        self.assertEqual(ipk.name, "org.example.demo_1.2.3_x86_64.ipk")
        members = wnk_ipk.read_ar(ipk.read_bytes())
        self.assertEqual([n for n, _ in members], ["debian-binary", "control.tar.gz", "data.tar.gz"])
        payload = wnk_ipk.read_payload(ipk)
        self.assertEqual(payload.app_id, "org.example.demo")
        info = json.loads(payload.files["usr/palm/packages/org.example.demo/packageinfo.json"])
        self.assertEqual((info["app"], info["id"], info["version"]), ("org.example.demo",) * 2 + ("1.2.3",))
        self.assertEqual(payload.modes[payload.main_path], 0o755)
        self.assertEqual(payload.modes["usr/palm/applications/org.example.demo/icon.png"], 0o644)
        with tarfile.open(fileobj=io.BytesIO(dict(members)["data.tar.gz"]), mode="r:gz") as tf:
            for m in tf.getmembers():
                self.assertEqual((m.uid, m.gid, m.uname, m.gname, m.mtime), (0, 0, "root", "root", mkipk.EPOCH))

    def test_rejects_bad_version_id_and_symlink(self):
        with self.assertRaises(mkipk.PackageError):
            build(make_stage(self.tmp / "v", version="1.2"), self.tmp / "o")
        with self.assertRaises(mkipk.PackageError):
            build(make_stage(self.tmp / "i", app_id="Org.Example"), self.tmp / "o")
        stage = make_stage(self.tmp / "s")
        os.symlink("demo", stage / "link")
        with self.assertRaises(mkipk.PackageError):
            build(stage, self.tmp / "o")


class CheckPackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wnk-chk-"))

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_check(self, ipk: Path, expected=None):
        with redirect_stdout(io.StringIO()) as out:
            failures = check_package.check_ipk(ipk, expected, "x86_64")
        return failures, out.getvalue()

    def test_good_package_passes(self):
        ipk = build(make_stage(self.tmp), self.tmp / "out")
        needed = wnk_elf.parse(HOST_ELF.read_bytes()).needed
        failures, out = self.run_check(ipk, needed)
        self.assertEqual(failures, [], out)

    def test_dt_needed_mismatch_fails(self):
        ipk = build(make_stage(self.tmp), self.tmp / "out")
        failures, _ = self.run_check(ipk, ["libnothing.so.1"])
        self.assertTrue(any("DT_NEEDED" in f for f in failures))

    def test_gnu_ar_trailing_slash_fails(self):
        ipk = build(make_stage(self.tmp), self.tmp / "out")
        blob = ipk.read_bytes().replace(b"debian-binary   ", b"debian-binary/  ", 1)
        bad = self.tmp / "bad.ipk"
        bad.write_bytes(blob)
        failures, _ = self.run_check(bad)
        self.assertTrue(any("bare names" in f for f in failures))

    def test_denied_bundled_library_fails(self):
        stage = make_stage(self.tmp)
        libc = Path("/lib/x86_64-linux-gnu/libc.so.6")
        if not libc.exists():
            self.skipTest("host libc not at the Debian/Ubuntu path")
        shutil.copyfile(os.path.realpath(libc), stage / "lib" / "libc.so.6")
        failures, _ = self.run_check(build(stage, self.tmp / "out"))
        self.assertTrue(any("allowed to be bundled" in f for f in failures))
        self.assertTrue(any("RPATH" in f for f in failures))


class FwCheckTests(unittest.TestCase):
    """fw-check against a synthetic inventory in the real format: symbols are "name@VERSION" and a
    library's version definitions appear as "VER@VER" entries."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wnk-fw-"))
        self.db = self.tmp / "db"
        host = wnk_elf.parse(HOST_ELF.read_bytes())
        self.needed = host.needed
        self.undefined = sorted({s.name for s in host.undefined if s.binding != wnk_elf.STB_WEAK})
        self.versions: dict[str, set[str]] = {}
        for lib, ver in host.version_needs:
            self.versions.setdefault(lib, set()).add(ver)
        self.missing_symbol = self.undefined[0]
        # The newest version the binary needs from any library: dropped in the "OLD" firmware.
        self.lib_with_version, vers = next(iter(sorted(self.versions.items())))
        self.dropped_version = max(vers, key=lambda v: tuple(int(x) for x in re.findall(r"\d+", v) or [0]))
        self.write_fw("01.00.00.01-FULL", "6.4.0", self.undefined, {})
        self.write_fw("02.00.00.01-PART", "7.4.0", self.undefined[1:], {})
        self.write_fw("03.00.00.01-OLD", "8.3.0", self.undefined, {self.lib_with_version: self.dropped_version})
        self.ipk = build(make_stage(self.tmp), self.tmp / "out")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write_fw(self, name, release, symbols, drop_version):
        d = self.db / name
        d.mkdir(parents=True)
        version, ota = name.split("-", 1)
        (d / "info.json").write_text(json.dumps({"version": version, "ota_id": ota, "release": release}))
        index = {}
        for i, lib in enumerate(self.needed):
            vers = sorted(v for v in self.versions.get(lib, set()) if drop_version.get(lib) != v)
            entries = [f"{v}@{v}" for v in vers]
            if i == 0:
                entries += [f"{sym}@{vers[0]}" if vers else sym for sym in symbols]
            index[lib] = f"lib{i}.json"
            (d / f"lib{i}.json").write_text(json.dumps({"name": lib, "package": "x", "needed": [],
                                                        "symbols": entries}))
        (d / "index.json").write_text(json.dumps(index))

    def run_fw(self, *extra):
        with redirect_stdout(io.StringIO()) as out:
            rc = fw_check.main([str(self.ipk), "--db", str(self.db), *extra])
        return rc, out.getvalue()

    def test_reports_per_release(self):
        rc, text = self.run_fw("--min-release", "1.0.0")
        self.assertEqual(rc, 1, text)
        self.assertIn("OK   webOS 6.4.0", text)
        self.assertIn("FAIL webOS 7.4.0", text)
        self.assertIn(self.missing_symbol, text)

    def test_missing_symbol_version_fails(self):
        rc, text = self.run_fw("--min-release", "8.0.0")
        self.assertEqual(rc, 1, text)
        self.assertIn(f"version {self.dropped_version}", text)

    def test_release_range_filter(self):
        rc, text = self.run_fw("--min-release", "6.0.0", "--max-release", "6.9.9")
        self.assertEqual(rc, 0, text)

    def test_no_db_is_exit_4(self):
        with redirect_stdout(io.StringIO()):
            rc = fw_check.main([str(self.ipk), "--db", str(self.tmp / "nope")])
        self.assertEqual(rc, 4)


class FwDbTests(unittest.TestCase):
    def test_release_key_orders_numerically(self):
        self.assertLess(wnk_fwdb.release_key("4.10.0"), wnk_fwdb.release_key("5.3.1"))
        self.assertLess(wnk_fwdb.release_key("9.2.0"), wnk_fwdb.release_key("10.2.0"))


if __name__ == "__main__":
    unittest.main()
