#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a kit crash record (crash.log / crash.log.prev from /tmp/<app id>/) into function names and
source lines, using the unstripped executable from the build tree.

    tools/symbolize-crash.py CRASH_LOG BINARY [--addr2line PATH]

BINARY is the unstripped executable (e.g. build/webos-release/applications/examples/tvui/wnk-tvui);
the ipk carries a stripped copy. The record's build id is compared with BINARY's so a record from a
different build is not silently mis-symbolised. Addresses are taken from the pc/lr line and from
backtrace frames that belong to BINARY; PIE executables are rebased with the record's maps lines.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# glibc prints "path(sym+off)[addr]", "path(+off)[addr]" (PIE) or "path[addr]" (no symbol, non-PIE).
FRAME = re.compile(r"^(?P<path>[^(\[]+)(\((?P<sym>[^)]*)\))?\s*\[(?P<addr>0x[0-9a-fA-F]+)\]")
MAP = re.compile(r"^(?P<start>[0-9a-f]+)-(?P<end>[0-9a-f]+)\s+r-xp\s+(?P<off>[0-9a-f]+)\s+\S+\s+\S+\s+(?P<path>\S+)")
REG = re.compile(r"\b(pc|lr)=(0x[0-9a-f]+)")


def elf_info(path: Path) -> tuple[bool, str, int]:
    """(is_pie, build_id hex, e_machine) without external tools."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"symbolize-crash: {path} is not an ELF file")
    is64 = data[4] == 2
    little = data[5] == 1
    order = "little" if little else "big"
    e_type = int.from_bytes(data[16:18], order)
    machine = int.from_bytes(data[18:20], order)
    build_id = ""
    # Walk section headers for .note.gnu.build-id (type NOTE, note type 3).
    if is64:
        shoff, shentsize, shnum = int.from_bytes(data[40:48], order), int.from_bytes(data[58:60], order), int.from_bytes(data[60:62], order)
    else:
        shoff, shentsize, shnum = int.from_bytes(data[32:36], order), int.from_bytes(data[46:48], order), int.from_bytes(data[48:50], order)
    for i in range(shnum):
        sh = data[shoff + i * shentsize: shoff + (i + 1) * shentsize]
        sh_type = int.from_bytes(sh[4:8], order)
        if sh_type != 7:  # SHT_NOTE
            continue
        off = int.from_bytes(sh[24:32] if is64 else sh[16:20], order)
        size = int.from_bytes(sh[32:40] if is64 else sh[20:24], order)
        p, end = off, off + size
        while p + 12 <= end:
            namesz, descsz, ntype = (int.from_bytes(data[p + k:p + k + 4], order) for k in (0, 4, 8))
            name = data[p + 12:p + 12 + namesz]
            desc_at = p + 12 + ((namesz + 3) & ~3)
            if ntype == 3 and name.rstrip(b"\0") == b"GNU":
                build_id = data[desc_at:desc_at + descsz].hex()
            p = desc_at + ((descsz + 3) & ~3)
    return e_type == 3, build_id, machine


def find_addr2line(explicit: str | None, machine: int) -> str:
    if explicit:
        return explicit
    if machine != 40:  # not ARM: the host's own binutils
        found = shutil.which("addr2line")
        if not found:
            raise SystemExit("symbolize-crash: no addr2line (install binutils)")
        return found
    ndk = os.environ.get("WEBOS_NDK", str(Path.home() / "webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot"))
    cand = Path(ndk) / "bin/arm-webos-linux-gnueabi-addr2line"
    if cand.exists():
        return str(cand)
    found = shutil.which("addr2line")
    if not found:
        raise SystemExit("symbolize-crash: no addr2line (install binutils or set WEBOS_NDK)")
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("crash_log", type=Path)
    ap.add_argument("binary", type=Path)
    ap.add_argument("--addr2line")
    args = ap.parse_args()

    text = args.crash_log.read_text(errors="replace")
    is_pie, build_id, machine = elf_info(args.binary)
    m = re.search(r"build_id=(\S*)", text)
    rec_id = m.group(1) if m else ""
    if rec_id and build_id and rec_id != build_id:
        print(f"symbolize-crash: build id mismatch: record {rec_id}, binary {build_id}", file=sys.stderr)
        return 2
    name = args.binary.name

    base = None  # load address of the binary's text mapping (PIE only)
    for line in text.splitlines():
        mm = MAP.match(line)
        if mm and os.path.basename(mm.group("path")) == name:
            base = int(mm.group("start"), 16) - int(mm.group("off"), 16)
            break

    def to_file(addr: int) -> int:
        return addr - base if (is_pie and base is not None) else addr

    wanted: list[tuple[str, int]] = []
    for line in text.splitlines():
        for reg, val in REG.findall(line):
            wanted.append((reg, to_file(int(val, 16))))
        fm = FRAME.match(line)
        if fm and os.path.basename(fm.group("path").strip()) == name:
            sym = fm.group("sym") or ""
            rel = re.match(r"^\+(0x[0-9a-f]+)$", sym)
            addr = int(rel.group(1), 16) if rel else to_file(int(fm.group("addr"), 16))
            wanted.append(("frame", addr))

    print(next((l for l in text.splitlines() if l.startswith("signal=")), "signal=?"))
    if not wanted:
        print("no addresses in the record belong to", name)
        return 1
    tool = find_addr2line(args.addr2line, machine)
    out = subprocess.run([tool, "-f", "-C", "-p", "-e", str(args.binary)] + [hex(a) for _, a in wanted],
                         capture_output=True, text=True, check=False)
    if out.returncode != 0 or not out.stdout:
        print(f"symbolize-crash: {tool} failed: {out.stderr.strip()}", file=sys.stderr)
        return 1
    for (kind, addr), res in zip(wanted, out.stdout.splitlines()):
        print(f"{kind:5} {addr:#010x}  {res}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
