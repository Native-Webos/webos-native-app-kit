#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Structure and policy checks modelled on PlxNative ci/check-package.py and ci/check-packaged-elf.py
# (GPL-3.0-or-later, https://github.com/GLinnik21/plx-native), rewritten for any kit application.
"""Check a built .ipk for structural and policy problems. Stdlib only; no NDK, no TV.

    tools/check-package.py PACKAGE.ipk [--expected-needed FILE] [--arch arm]

What a pass means: the archive is shaped the way LG's installer and webosbrew's tools expect,
metadata agrees everywhere, every ELF matches the target ABI, bundled libraries follow the kit's
bundling policy, the executable needs exactly the reviewed set of system libraries, and licenses/
carries the GPL text, the notices, the source note and the licence of every bundled library.
What it does NOT mean: that the app loads on a given firmware (tools/fw-check.py) or works on a TV.
"""
from __future__ import annotations

import argparse
import io
import json
import posixpath
import re
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wnk_elf  # noqa: E402
from wnk_ipk import read_ar  # noqa: E402

# Libraries that must come from the TV and never ride in a package (docs/research/PACKAGING-AND-FIRMWARE.md A.3).
DENY_BUNDLED = [
    r"libstdc\+\+\.so.*",  # bundled copy shadows the TV's for LG libraries (punktfunk cc-shim.sh)
    r"libgcc_s\.so.*",
    r"libc\.so.*", r"libm\.so.*", r"libdl\.so.*", r"libpthread\.so.*", r"librt\.so.*", r"ld-linux.*",
    r"libNDL_.*", r"libndl-.*", r"libplayerAPIs.*", r"libpf-.*", r"libAcb.*", r"liblgncopenapi.*",
    r"libEGL\.so.*", r"libGLES.*", r"libwayland-.*", r"libluna-service2.*", r"libhelpers\.so.*",
    r"libpbnjson.*", r"libPmLog.*", r"libumedia.*", r"libresource_mgr.*", r"libums_.*",
]
# A private runtime (--private-runtime DIR) is a self-contained tree run as a separate process through its
# own bundled loader, e.g. a JVM/ART with its own C library. It never shares an address space with LG's
# libraries, so the libstdc++/libgcc/libc/EGL rules above (which exist because a bundled copy would shadow
# the TV's for LG code) do not apply inside it. LG firmware libraries are still never allowed.
DENY_IN_PRIVATE_RUNTIME = [
    r"libNDL_.*", r"libndl-.*", r"libplayerAPIs.*", r"libpf-.*", r"libAcb.*", r"liblgncopenapi.*",
    r"libGLES.*", r"libwayland-.*", r"libluna-service2.*", r"libhelpers\.so.*", r"libpbnjson.*", r"libPmLog.*",
    r"libumedia.*", r"libresource_mgr.*", r"libums_.*",
]
EM_AARCH64 = 183
PRIVATE_RUNTIME_MACHINES = {"arm": {wnk_elf.EM_ARM, EM_AARCH64}, "x86_64": {wnk_elf.EM_X86_64}}
MAINTAINER_RE = re.compile(r"[^<>]+ <[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}>")
VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")


class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def check(self, cond: bool, msg: str) -> bool:
        print(f"  {'ok  ' if cond else 'FAIL'} {msg}")
        if not cond:
            self.failures.append(msg)
        return cond


def parse_control(text: str) -> dict[str, str]:
    fields = {}
    for line in text.splitlines():
        if ":" in line and not line.startswith(" "):
            k, v = line.split(":", 1)
            fields[k.strip()] = v.strip()
    return fields


def check_private_runtimes(r: Report, elfs: dict[str, wnk_elf.ElfInfo], app_root: str, runtimes: list[str],
                           arch: str) -> None:
    """Self-contained trees: every DT_NEEDED resolves inside the tree, no LG library, nothing outside
    the tree loads from it (no RPATH/RUNPATH of another ELF names it)."""
    for rt in runtimes:
        rt = rt.strip("/")
        prefix = f"{app_root}{rt}/"
        inside = {n: i for n, i in elfs.items() if n.startswith(prefix)}
        if not r.check(bool(inside), f"{rt}/: private runtime contains ELF files"):
            continue
        provided = set()
        for name, info in inside.items():
            provided.add(posixpath.basename(name))
            if info.soname:
                provided.add(info.soname)
        for name, info in sorted(inside.items()):
            short = name[len(app_root):]
            r.check(info.machine in PRIVATE_RUNTIME_MACHINES.get(arch, {info.machine}),
                    f"{short}: machine {info.machine} allowed in a private runtime for {arch}")
            missing = [n for n in info.needed if n not in provided]
            r.check(not missing, f"{short}: every DT_NEEDED is inside {rt}/" + (f" (missing {missing})" if missing else ""))
            base = {posixpath.basename(name), info.soname or ""}
            r.check(not any(re.fullmatch(p, b) for p in DENY_IN_PRIVATE_RUNTIME for b in base if b),
                    f"{short}: not an LG firmware library")
        for name, info in sorted(elfs.items()):
            if name.startswith(prefix):
                continue
            leaks = [p for p in info.rpath + info.runpath if rt in p.split("/")]
            r.check(not leaks, f"{name[len(app_root):]}: does not load libraries from {rt}/"
                    + (f" (rpath {leaks})" if leaks else ""))


def check_ipk(path: Path, expected_needed: list[str] | None, arch: str,
              private_runtimes: list[str] | None = None) -> list[str]:
    r = Report()
    print(f"check-package: {path}")
    members = read_ar(path.read_bytes())
    names = [n for n, _ in members]
    r.check(names == ["debian-binary", "control.tar.gz", "data.tar.gz"],
            f"ar members are debian-binary, control.tar.gz, data.tar.gz in order with bare names (got {names})")
    blobs = dict(members)
    r.check(blobs.get("debian-binary") == b"2.0\n", "debian-binary is '2.0\\n'")
    if "control.tar.gz" not in blobs or "data.tar.gz" not in blobs:
        return r.failures + ["missing archive members"]

    with tarfile.open(fileobj=io.BytesIO(blobs["control.tar.gz"]), mode="r:gz") as tf:
        ctl_member = next((m for m in tf.getmembers() if m.name.lstrip("./") == "control"), None)
        r.check(ctl_member is not None, "control.tar.gz contains 'control'")
        control = parse_control(tf.extractfile(ctl_member).read().decode("utf-8")) if ctl_member else {}

    for key in ("Package", "Version", "Architecture", "Installed-Size", "Maintainer", "Description"):
        r.check(bool(control.get(key)), f"control has {key}")
    r.check(control.get("Architecture") == arch, f"control Architecture is {arch}")
    r.check(bool(MAINTAINER_RE.fullmatch(control.get("Maintainer", ""))), "Maintainer is 'Name <email>'")
    r.check(bool(VERSION_RE.match(control.get("Version", ""))), "Version is three integers")
    app_id = control.get("Package", "")
    app_root = f"usr/palm/applications/{app_id}/"

    files: dict[str, bytes] = {}
    modes: dict[str, int] = {}
    total = 0
    unsafe, links, specials, not_root = [], [], [], []
    with tarfile.open(fileobj=io.BytesIO(blobs["data.tar.gz"]), mode="r:gz") as tf:
        for m in tf.getmembers():
            name = m.name[2:] if m.name.startswith("./") else m.name
            norm = posixpath.normpath(name)
            if (name.startswith("/") or ".." in norm.split("/")
                    or not (norm in ("usr", "usr/palm") or norm.startswith("usr/palm/"))):
                unsafe.append(name)
                continue
            if m.uid != 0 or m.gid != 0:
                not_root.append(name)
            if m.issym() or m.islnk():
                links.append(name)
            elif m.isfile():
                files[norm] = tf.extractfile(m).read()
                modes[norm] = m.mode & 0o7777
                total += m.size
            elif not m.isdir():
                specials.append(name)
    r.check(not unsafe, "every payload path is under usr/palm/ and relative"
            + (f" (bad: {unsafe[:5]})" if unsafe else ""))
    r.check(not links, "payload has no symlinks or hard links" + (f" (found: {links[:5]})" if links else ""))
    r.check(not specials, "payload has only files and directories" + (f" (found: {specials[:5]})" if specials else ""))
    r.check(not not_root, "payload is owned by root:root" + (f" (not: {not_root[:5]})" if not_root else ""))

    kib = int(control.get("Installed-Size", "0") or 0)
    r.check(abs(kib - (total + 1023) // 1024) <= 1, f"Installed-Size {kib} KiB matches payload ({total} bytes)")

    appinfo_raw = files.get(app_root + "appinfo.json")
    if not r.check(appinfo_raw is not None, f"{app_root}appinfo.json present"):
        return r.failures
    appinfo = json.loads(appinfo_raw)
    r.check(appinfo.get("id") == app_id, "appinfo id equals control Package and directory name")
    r.check(appinfo.get("version") == control.get("Version"), "appinfo version equals control Version")
    r.check(appinfo.get("type") == "native", "appinfo type is native")
    pkginfo_raw = files.get(f"usr/palm/packages/{app_id}/packageinfo.json")
    if r.check(pkginfo_raw is not None, "packageinfo.json present (required by appInstallService)"):
        pkginfo = json.loads(pkginfo_raw)
        r.check(pkginfo.get("app") == app_id and pkginfo.get("id") == app_id, "packageinfo app/id match")
        r.check(pkginfo.get("version") == control.get("Version"), "packageinfo version matches")
    for icon_key in ("icon", "largeIcon"):
        if icon_key in appinfo:
            r.check(app_root + appinfo[icon_key] in files, f"{icon_key} '{appinfo[icon_key]}' present")

    main_path = posixpath.normpath(app_root + appinfo.get("main", ""))
    main_blob = files.get(main_path)
    if not r.check(main_blob is not None, f"main executable {appinfo.get('main')} present"):
        return r.failures
    r.check(modes.get(main_path, 0) & 0o111 != 0, "main executable has exec permission")

    bundled_dir = app_root + "lib/"
    bundled = {p[len(bundled_dir):]: b for p, b in files.items()
               if p.startswith(bundled_dir) and "/" not in p[len(bundled_dir):]}
    private_runtimes = private_runtimes or []
    private_prefixes = tuple(f"{app_root}{rt.strip('/')}/" for rt in private_runtimes)
    elfs: dict[str, wnk_elf.ElfInfo] = {}
    for name, blob in sorted(files.items()):
        if not wnk_elf.is_elf(blob):
            continue
        info = wnk_elf.parse(blob)
        elfs[name] = info
        if private_prefixes and name.startswith(private_prefixes):
            continue  # checked by check_private_runtimes
        short = name[len(app_root):] if name.startswith(app_root) else name
        if arch == "arm":
            r.check(info.elf_class == 32 and info.little_endian and info.machine == wnk_elf.EM_ARM,
                    f"{short}: ELF32 little-endian ARM")
            r.check(info.arm_eabi_version == 5 and info.arm_hard_float is False,
                    f"{short}: EABI5 soft-float calling convention (arm-webos-linux-gnueabi)")
        if name.startswith(bundled_dir):
            base = posixpath.basename(name)
            r.check(info.soname == base, f"{short}: staged under its SONAME ({info.soname})")
            r.check(not any(re.fullmatch(p, base) for p in DENY_BUNDLED),
                    f"{short}: allowed to be bundled (not a firmware/runtime library)")
        r.check(not any(lib.startswith("libstdc++") for lib in info.needed),
                f"{short}: does not need libstdc++.so (link it statically)")
    check_private_runtimes(r, elfs, app_root, private_runtimes, arch)

    # Licences (cmake/WebOSPackage.cmake stages them): a package must say what it is under and where
    # its source is, and carry the licence of everything it bundles.
    lic = app_root + "licenses/"
    for need in ("LICENSE", "THIRD-PARTY-NOTICES.md", "SOURCE.txt"):
        r.check(lic + need in files, f"licenses/{need} present")
    if "libSDL2-2.0.so.0" in bundled:
        r.check(lic + "SDL2-zlib.txt" in files, "licenses/SDL2-zlib.txt present (SDL is bundled)")
    if any("-wnk.so" in n for n in bundled):
        r.check(lic + "FFmpeg-LGPL-2.1.txt" in files, "licenses/FFmpeg-LGPL-2.1.txt present (FFmpeg is bundled)")
        source = files.get(lic + "SOURCE.txt", b"").decode("utf-8", "replace")
        r.check("ffmpeg.org/releases/ffmpeg-" in source and "sha256" in source,
                "licenses/SOURCE.txt names the FFmpeg source tarball and its checksum")

    main_info = wnk_elf.parse(main_blob)
    paths = main_info.rpath + main_info.runpath
    if bundled:
        r.check("$ORIGIN/lib" in paths, f"main has RPATH/RUNPATH $ORIGIN/lib for bundled libraries (has {paths})")
    if expected_needed is not None:
        actual = sorted(main_info.needed)
        want = sorted(expected_needed)
        r.check(actual == want, "main DT_NEEDED equals the reviewed list"
                + ("" if actual == want else f"\n         missing: {sorted(set(want) - set(actual))}"
                   f"\n         unexpected: {sorted(set(actual) - set(want))}"))
    return r.failures


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ipk", type=Path)
    ap.add_argument("--expected-needed", type=Path, help="file listing the executable's DT_NEEDED, one per line")
    ap.add_argument("--arch", default="arm")
    ap.add_argument("--private-runtime", action="append", default=[], metavar="DIR",
                    help="app-relative directory holding a self-contained runtime run as a separate process "
                         "(see DENY_IN_PRIVATE_RUNTIME); repeatable")
    args = ap.parse_args(argv)
    expected = None
    if args.expected_needed:
        expected = [ln.split("#", 1)[0].strip() for ln in args.expected_needed.read_text().splitlines()
                    if ln.split("#", 1)[0].strip()]
    try:
        failures = check_ipk(args.ipk, expected, args.arch, args.private_runtime)
    except (OSError, ValueError, tarfile.TarError, wnk_elf.ElfError) as exc:
        print(f"check-package: cannot read {args.ipk}: {exc}", file=sys.stderr)
        return 3
    if failures:
        print(f"check-package: {len(failures)} problem(s)")
        return 1
    print("check-package: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
