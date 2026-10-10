#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Would this .ipk LOAD on each retail firmware? Offline, against the webosbrew inventory.

    tools/fw-check.py PACKAGE.ipk [--min-release 5.3.1] [--max-release X] [--db DIR]
                      [--runtime-libs FILE] [--verbose]

For every firmware image in the release range it emulates what the dynamic loader needs:
  1. Resolve the executable's DT_NEEDED breadth-first: bundled lib/ first (when the executable's
     RPATH/RUNPATH includes $ORIGIN/lib), then the firmware's libraries.
  2. Every non-weak undefined dynamic symbol of the executable and of each bundled library must be
     exported by some library in that load set.
  3. Every symbol version the ELF requires from a firmware library (.gnu.version_r, e.g.
     libc.so.6 GLIBC_2.4) must be defined by that library in the inventory. Any GLIBCXX/CXXABI
     requirement is reported, since the kit links libstdc++ statically.

  4. With --runtime-libs, every SONAME listed (libraries loaded with dlopen() at runtime, invisible
     to DT_NEEDED) must exist in the firmware. Presence only; their symbols are not checked.

Exit 0 = loadable on every release in range, 1 = a problem on at least one, 3 = unreadable input,
4 = no firmware data. A pass means "the loader would start it", never "it works".
Same question as webosbrew-ipk-verify; this is an independent second implementation.
"""
from __future__ import annotations

import argparse
import posixpath
import re
import sys
from collections import deque
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wnk_elf  # noqa: E402
import wnk_fwdb  # noqa: E402
import wnk_ipk  # noqa: E402


def version_tuple(v: str) -> tuple[int, ...]:
    return tuple(int(x) for x in re.findall(r"\d+", v))


def check_firmware(fw: wnk_fwdb.Firmware, main: wnk_elf.ElfInfo, bundled: dict[str, wnk_elf.ElfInfo],
                   use_bundled: bool, runtime_libs: list[str] = ()) -> tuple[list[str], list[str]]:
    """Returns (errors, notes) for one firmware image."""
    errors: list[str] = []
    notes: list[str] = []
    provided: set[str] = set()
    seen: set[str] = set()
    queue = deque(main.needed)
    for b in bundled.values():
        queue.extend(b.needed)
    while queue:
        name = queue.popleft()
        if name in seen:
            continue
        seen.add(name)
        if use_bundled and name in bundled:
            info = bundled[name]
            provided.update(s.name for s in info.exported)
            queue.extend(info.needed)
            continue
        rec = fw.lib(name)
        if rec is None:
            errors.append(f"missing library {name}")
            continue
        provided.update(rec.symbols)
        queue.extend(rec.needed)

    for owner, info in [("main", main)] + [(n, i) for n, i in bundled.items()]:
        missing = sorted({s.name for s in info.undefined
                          if s.binding != wnk_elf.STB_WEAK and s.name not in provided})
        if missing:
            shown = ", ".join(missing[:8]) + (f" (+{len(missing) - 8} more)" if len(missing) > 8 else "")
            errors.append(f"{owner}: {len(missing)} unresolved symbol(s): {shown}")

    for owner, info in [("main", main)] + [(n, i) for n, i in bundled.items()]:
        for lib, ver in info.version_needs:
            if ver.startswith(("GLIBCXX_", "CXXABI_")):
                errors.append(f"{owner}: needs {ver} from {lib}; link libstdc++ statically")
                continue
            if use_bundled and lib in bundled:
                continue
            rec = fw.lib(lib)
            if rec is None:
                continue  # already reported as a missing library
            if not rec.versions:
                notes.append(f"{lib}: inventory records no symbol versions; {ver} unchecked")
            elif ver not in rec.versions:
                errors.append(f"{owner}: needs {lib} version {ver}, which this firmware's {lib} does not define")
    for lib in runtime_libs:
        if fw.lib(lib) is None:
            errors.append(f"runtime-loaded library {lib} is not present")
    return errors, notes


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ipk", type=Path)
    ap.add_argument("--min-release", default="5.3.1")
    ap.add_argument("--max-release")
    ap.add_argument("--db")
    ap.add_argument("--runtime-libs", type=Path, help="file listing SONAMEs loaded with dlopen() at runtime")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    db = wnk_fwdb.find_db(args.db)
    if db is None:
        print("fw-check: no firmware inventory found. Install webosbrew-toolbox-fw-symbols "
              "(https://github.com/webosbrew/dev-toolbox-cli/releases) or set WEBOS_FWSYM_DB.",
              file=sys.stderr)
        return 4
    try:
        payload = wnk_ipk.read_payload(args.ipk)
        main_info = wnk_elf.parse(payload.files[payload.main_path])
        bundled = {}
        lib_prefix = payload.root + "lib/"
        for path, blob in payload.files.items():
            if path.startswith(lib_prefix) and "/" not in path[len(lib_prefix):] and wnk_elf.is_elf(blob):
                info = wnk_elf.parse(blob)
                bundled[info.soname or posixpath.basename(path)] = info
    except (OSError, ValueError, KeyError, wnk_elf.ElfError) as exc:
        print(f"fw-check: cannot read {args.ipk}: {exc}", file=sys.stderr)
        return 3

    runtime_libs: list[str] = []
    if args.runtime_libs:
        runtime_libs = [ln.split("#", 1)[0].strip() for ln in args.runtime_libs.read_text().splitlines()
                        if ln.split("#", 1)[0].strip()]
    use_bundled = "$ORIGIN/lib" in (main_info.rpath + main_info.runpath)
    lo = wnk_fwdb.release_key(args.min_release)
    hi = wnk_fwdb.release_key(args.max_release) if args.max_release else None
    firmwares = [f for f in wnk_fwdb.load(db)
                 if wnk_fwdb.release_key(f.release) >= lo and (hi is None or wnk_fwdb.release_key(f.release) <= hi)]
    if not firmwares:
        print(f"fw-check: no firmware in range in {db}", file=sys.stderr)
        return 4

    print(f"fw-check: {args.ipk.name} against {len(firmwares)} firmware image(s) from {db}")
    if runtime_libs:
        print(f"          runtime-loaded (dlopen) libraries checked: {len(runtime_libs)}")
    print(f"          main needs {main_info.needed}; bundled {sorted(bundled)}"
          f"{'' if use_bundled or not bundled else ' (NOT on the RPATH!)'}")
    failed = 0
    for fw in firmwares:
        errors, notes = check_firmware(fw, main_info, bundled, use_bundled, runtime_libs)
        status = "OK  " if not errors else "FAIL"
        print(f"  {status} webOS {fw.release:<8} {fw.version}-{fw.ota_id}")
        for e in errors:
            print(f"         - {e}")
        if args.verbose:
            for n in notes:
                print(f"         · {n}")
        failed += bool(errors)
    print(f"fw-check: {len(firmwares) - failed}/{len(firmwares)} loadable "
          "(symbol presence only; behaviour needs a TV)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
