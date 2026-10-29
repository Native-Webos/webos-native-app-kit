#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from PlxNative ci/mkipk.py (Copyright (c) 2026 Gleb Linnik, GPL-3.0-or-later),
# https://github.com/GLinnik21/plx-native — reduced to a generic, app-independent packager.
"""Build a webOS .ipk from a staged application directory, deterministically.

    tools/mkipk.py --app-dir STAGE --out-dir DIST [--maintainer "Name <mail@example.org>"]
                   [--description TEXT] [--license SPDX] [--homepage URL]

STAGE is the application directory as it should appear on the TV under
usr/palm/applications/<id>/ and must contain appinfo.json. The ipk is written to
DIST/<id>_<version>_<arch>.ipk and its sha256 is printed.

Why this exists instead of `ares-package` (PlxNative's findings, see
docs/research/PACKAGING-AND-FIRMWARE.md A.2):
  * Reproducible bytes: uid/gid/names/mtime/mode/order and the gzip header are normalised, so the
    same input always gives the same sha256 (the Homebrew manifest hash is the only integrity
    check in the webosbrew chain).
  * `ar` members are written with bare names. GNU ar appends '/', which appinstalld rejects
    ("Failed to extract package", error -5).
  * usr/palm/packages/<id>/packageinfo.json is generated; without it an install registers nothing.
  * Installed-Size is in KiB, as opkg expects.
  * <app>/.wnk-state/ is added with mode 1777: the only directory the jailed app can write, used by
    the kit to tell a reinstall from an update (everything else is root:root 0755/0644).
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import stat
import sys
import tarfile
from pathlib import Path

EPOCH = 1262304000  # fixed mtime (2010-01-01) for every archive member
PACKAGER = "wnk-mkipk/1"
STATE_DIR = ".wnk-state"
APP_ID_RE = re.compile(r"^[a-z0-9]+(?:[.-][a-z0-9]+)+$")
VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")


class PackageError(Exception):
    pass


def read_appinfo(app_dir: Path) -> dict:
    path = app_dir / "appinfo.json"
    if not path.is_file():
        raise PackageError(f"{path} is missing")
    try:
        app = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as exc:
        raise PackageError(f"{path} is not valid JSON: {exc}") from exc
    for key in ("id", "version", "type", "main", "title", "vendor"):
        if not isinstance(app.get(key), str) or not app[key]:
            raise PackageError(f"appinfo.json needs a non-empty string '{key}'")
    if not APP_ID_RE.match(app["id"]):
        raise PackageError(f"app id {app['id']!r} must be lowercase reverse-DNS (a-z, 0-9, '.', '-')")
    if not VERSION_RE.match(app["version"]):
        raise PackageError(f"version {app['version']!r} must be exactly three integers (x.y.z)")
    main = app_dir / app["main"]
    if not main.is_file():
        raise PackageError(f"appinfo.json main {app['main']!r} does not exist in {app_dir}")
    return app


def collect_files(app_dir: Path) -> list[Path]:
    files = []
    for p in sorted(app_dir.rglob("*"), key=lambda p: p.relative_to(app_dir).as_posix()):
        st = p.lstat()
        if stat.S_ISLNK(st.st_mode):
            raise PackageError(f"{p} is a symlink; stage real files under their final names")
        if not (stat.S_ISREG(st.st_mode) or stat.S_ISDIR(st.st_mode)):
            raise PackageError(f"{p} is not a regular file or directory")
        files.append(p)
    return files


def tar_entry(name: str, *, is_dir: bool = False, size: int = 0, mode: int = 0o644) -> tarfile.TarInfo:
    ti = tarfile.TarInfo(name)
    ti.type = tarfile.DIRTYPE if is_dir else tarfile.REGTYPE
    ti.size = 0 if is_dir else size
    ti.mode = mode
    ti.mtime = EPOCH
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    return ti


def targz(entries: list[tuple[str, bytes | None, int]]) -> bytes:
    """entries: (archive name, file bytes or None for a directory, mode). Order is preserved."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for name, blob, mode in entries:
            if blob is None:
                tf.addfile(tar_entry(name, is_dir=True, mode=mode))
            else:
                tf.addfile(tar_entry(name, size=len(blob), mode=mode), io.BytesIO(blob))
    out = io.BytesIO()
    # mtime=0 and an empty name keep the gzip header itself deterministic.
    with gzip.GzipFile(filename="", mode="wb", fileobj=out, mtime=0, compresslevel=9) as gz:
        gz.write(raw.getvalue())
    return out.getvalue()


def ar_archive(members: list[tuple[str, bytes]]) -> bytes:
    """Common-format ar with bare member names (no GNU trailing '/')."""
    out = io.BytesIO()
    out.write(b"!<arch>\n")
    for name, blob in members:
        if len(name) > 16 or "/" in name:
            raise PackageError(f"ar member name {name!r} not representable without a long-name table")
        out.write(f"{name:<16}{EPOCH:<12}{0:<6}{0:<6}{'100644':<8}{len(blob):<10}".encode("ascii"))
        out.write(b"`\n")
        out.write(blob)
        if len(blob) % 2:
            out.write(b"\n")
    return out.getvalue()


def build(app_dir: Path, out_dir: Path, maintainer: str, description: str | None,
          license_id: str | None, homepage: str | None, arch: str) -> Path:
    app = read_appinfo(app_dir)
    app_id, version = app["id"], app["version"]
    root = f"usr/palm/applications/{app_id}"

    data: list[tuple[str, bytes | None, int]] = []
    for d in ("usr", "usr/palm", "usr/palm/applications", root):
        data.append((d, None, 0o755))
    payload_bytes = 0
    for p in collect_files(app_dir):
        rel = p.relative_to(app_dir).as_posix()
        if p.is_dir():
            data.append((f"{root}/{rel}", None, 0o755))
        else:
            blob = p.read_bytes()
            payload_bytes += len(blob)
            executable = bool(p.stat().st_mode & 0o111)
            data.append((f"{root}/{rel}", blob, 0o755 if executable else 0o644))
    # The one directory the jailed app may write in: the kit's install marker (wnk/storage/kv.hpp,
    # reconcile_install). It goes with the app on uninstall and keeps its files across updates.
    # Sticky, so other apps (which share the jail group) cannot remove or rename the app's files.
    state = f"{root}/{STATE_DIR}"
    if any(name == state for name, _, _ in data):
        raise PackageError(f"{STATE_DIR}/ is the kit's; do not stage it")
    data.append((state, None, 0o1777))

    # Key order follows what ares-package writes (per PlxNative), so packages diff cleanly.
    packageinfo = (json.dumps({
        "app": app_id,
        "id": app_id,
        "loc_name": app["title"],
        "package_format_version": 2,
        "vendor": app["vendor"],
        "version": version,
    }, indent=4) + "\n").encode("utf-8")
    pkg_dir = f"usr/palm/packages/{app_id}"
    data += [("usr/palm/packages", None, 0o755), (pkg_dir, None, 0o755),
             (f"{pkg_dir}/packageinfo.json", packageinfo, 0o644)]
    payload_bytes += len(packageinfo)

    installed_kib = (payload_bytes + 1023) // 1024
    desc = (description or app.get("appDescription") or app["title"]).replace("\n", " ").strip()
    control_lines = [
        f"Package: {app_id}",
        f"Version: {version}",
        "Section: misc",
        "Priority: optional",
        f"Architecture: {arch}",
        f"Installed-Size: {installed_kib}",
        f"Maintainer: {maintainer}",
    ]
    if homepage:
        control_lines.append(f"Homepage: {homepage}")
    if license_id:
        control_lines.append(f"License: {license_id}")
    control_lines += [
        f"Description: {desc}",
        "webOS-Package-Format-Version: 2",
        f"webOS-Packager-Version: {PACKAGER}",
    ]
    control = ("\n".join(control_lines) + "\n").encode("utf-8")

    ipk = ar_archive([
        ("debian-binary", b"2.0\n"),
        ("control.tar.gz", targz([("control", control, 0o644)])),
        ("data.tar.gz", targz(data)),
    ])
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"{app_id}_{version}_{arch}.ipk"
    tmp = out.with_suffix(".ipk.tmp")
    tmp.write_bytes(ipk)
    os.replace(tmp, out)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app-dir", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--maintainer", default="webos-native-app-kit <noreply@example.org>")
    ap.add_argument("--description")
    ap.add_argument("--license", dest="license_id", default="GPL-3.0-or-later")
    ap.add_argument("--homepage")
    ap.add_argument("--arch", default="arm")
    args = ap.parse_args(argv)
    try:
        out = build(args.app_dir, args.out_dir, args.maintainer, args.description, args.license_id,
                    args.homepage, args.arch)
    except PackageError as exc:
        print(f"mkipk: {exc}", file=sys.stderr)
        return 1
    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    print(f"mkipk: wrote {out} ({out.stat().st_size} bytes)")
    print(f"{digest}  {out.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
