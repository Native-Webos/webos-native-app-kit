#!/usr/bin/env python3
"""Print which shared libraries (and which versions) exist in each retail webOS firmware image.

Reads a local extraction of the webosbrew firmware symbol inventory (the
`webosbrew-toolbox-fw-symbols` package from https://github.com/webosbrew/dev-toolbox-cli):
one directory per firmware, each with `info.json` and `index.json`.

The inventory proves a library/symbol EXISTS in a firmware image. It says nothing about
whether the API behaves correctly at runtime.

Usage:
    tools/fw-lib-matrix.py [--db DIR] [--lib SONAME ...] [--symbol SONAME:SYMBOL ...]
"""
import argparse
import json
import os
import re
import sys

DEFAULT_LIBS = [
    "ld-linux.so.3", "libc.so.6", "libstdc++.so.6",
    "libSDL2-2.0.so.0", "libEGL.so.1", "libGLESv2.so.2",
    "libwayland-client.so.0", "libwayland-webos-client.so.1",
    "libNDL_directmedia.so.1", "libplayerAPIs.so.1", "libAcbAPI.so.1",
    "libcurl.so.4", "libssl.so.1.0.0", "libssl.so.1.1", "libssl.so.3",
    "libpng16.so.16", "libjpeg.so.8", "libjpeg.so.62", "libfreetype.so.6",
    "libharfbuzz.so.0", "libz.so.1", "libsqlite3.so.0",
    "libpbnjson_c.so.2", "libluna-service2.so.3", "libPmLogLib.so.3", "libhelpers.so.2",
    "libpulse.so.0", "libasound.so.2", "libopus.so.0",
]


def candidate_dbs():
    env = os.environ.get("WEBOS_FWSYM_DB")
    if env:
        yield env
    cache = os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache"))
    yield os.path.join(cache, "webos-native-app-kit", "fwsym", "data")
    yield os.path.join(cache, "plxnative", "fwsym", "data")  # same upstream package, if already fetched


def release_key(rel):
    return [int(x) if x.isdigit() else 0 for x in rel.split(".")]


def load(db):
    rows = []
    for fw in os.listdir(db):
        path = os.path.join(db, fw)
        try:
            with open(os.path.join(path, "info.json")) as f:
                info = json.load(f)
            with open(os.path.join(path, "index.json")) as f:
                index = json.load(f)
        except (OSError, ValueError):
            continue
        rows.append((info.get("release", "?"), info.get("version", "?"), path, index))
    rows.sort(key=lambda r: release_key(r[0]))
    return rows


def lib_version(filename):
    stem = filename[:-5] if filename.endswith(".json") else filename
    m = re.search(r"\.so\.?(.*)$", stem)
    if m and m.group(1):
        return m.group(1)
    m = re.search(r"-([\d.]+)\.so", stem)
    return m.group(1) if m else "yes"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", help="firmware inventory directory (one subdirectory per firmware)")
    ap.add_argument("--lib", action="append", help="SONAME to report (repeatable)")
    ap.add_argument("--symbol", action="append", default=[], help="SONAME:SYMBOL presence check (repeatable)")
    args = ap.parse_args()

    db = args.db or next((d for d in candidate_dbs() if os.path.isdir(d)), None)
    if not db or not os.path.isdir(db):
        sys.exit("fw-lib-matrix: no firmware inventory found; pass --db or set WEBOS_FWSYM_DB "
                 "(source: webosbrew-toolbox-fw-symbols from webosbrew/dev-toolbox-cli releases)")
    rows = load(db)
    if not rows:
        sys.exit(f"fw-lib-matrix: {db} contains no firmware entries")

    width = 8
    label = max([30] + [len(x) + 1 for x in (args.lib or DEFAULT_LIBS) + args.symbol])
    print("webOS release".ljust(label) + "".join(r[0].ljust(width) for r in rows))
    for lib in args.lib or DEFAULT_LIBS:
        cells = []
        for _, _, _, index in rows:
            f = index.get(lib)
            cells.append((lib_version(f)[:width - 1] if f else "-").ljust(width))
        print(lib.ljust(label) + "".join(cells))
    for spec in args.symbol:
        lib, _, sym = spec.partition(":")
        cells = []
        for _, _, path, index in rows:
            f = index.get(lib)
            if not f:
                cells.append("nolib".ljust(width))
                continue
            with open(os.path.join(path, f)) as fh:
                syms = {e.partition("@")[0] for e in json.load(fh).get("symbols", [])}
            cells.append(("yes" if sym in syms else "NO").ljust(width))
        print(f"{lib}:{sym}".ljust(label) + "".join(cells))


if __name__ == "__main__":
    main()
