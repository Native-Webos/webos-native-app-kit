#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write RELEASE-NOTES.md for a release directory made by tools/release.sh.

    tools/release-notes.py RELEASE_DIR --version X.Y.Z --commit SHA [--fw-log FILE]...
                           [--records docs/compatibility/records.json]

The notes are generated from what is in the directory, so they cannot disagree with it: the
packages (id, version, size, bundled libraries, SHA-256, read from each .ipk), the firmware
loadability lines tools/fw-check.py printed for them, and the compatibility records (the device
results, grouped by device). Stdlib only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wnk_ipk import read_payload  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package_rows(release: Path) -> list[str]:
    rows = ["| Package | Version | Size | Bundled libraries | SHA-256 |", "|---|---|---|---|---|"]
    for ipk in sorted(release.glob("*.ipk")):
        p = read_payload(ipk)
        libs = sorted(n[len(p.root) + 4:] for n in p.files if n.startswith(p.root + "lib/"))
        rows.append(f"| `{p.app_id}` | {p.appinfo.get('version', '?')} | {ipk.stat().st_size / 1e6:.1f} MB | "
                    f"{', '.join(f'`{l}`' for l in libs) or '-'} | `{sha256(ipk)[:16]}…` |")
    return rows


def firmware_lines(logs: list[Path]) -> list[str]:
    out = []
    for log in logs:
        name = log.stem.replace(".fw", "")
        summary = [l.strip() for l in log.read_text().splitlines() if l.startswith("fw-check:") and "loadable" in l]
        out.append(f"- `{name}`: {summary[-1].removeprefix('fw-check: ') if summary else 'no result'}")
    return out


def compat_sections(records_path: Path) -> list[str]:
    data = json.loads(records_path.read_text())
    by_device: dict[str, list[dict]] = defaultdict(list)
    for r in data.get("records", []):
        by_device[r.get("device", "?")].append(r)
    out = []
    for device, records in sorted(by_device.items()):
        counts = defaultdict(int)
        for r in records:
            counts[r.get("result", "?")] += 1
        out.append(f"### {device}")
        out.append("")
        out.append(", ".join(f"{n} {k}" for k, n in sorted(counts.items())) + f" ({len(records)} records)")
        out.append("")
        out.append("| Feature | Result | Date | Limitations |")
        out.append("|---|---|---|---|")
        for r in sorted(records, key=lambda r: r.get("feature", "")):
            lim = (r.get("limitations") or "").replace("|", "/").replace("\n", " ")
            if len(lim) > 140:
                lim = lim[:137] + "…"
            out.append(f"| `{r.get('feature')}` | {r.get('result')} | {r.get('date')} | {lim} |")
        out.append("")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("release", type=Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--commit", required=True)
    ap.add_argument("--fw-log", type=Path, action="append", default=[])
    ap.add_argument("--records", type=Path, default=ROOT / "docs/compatibility/records.json")
    ap.add_argument("--reproducible", choices=["yes", "no", "unchecked"], default="unchecked")
    args = ap.parse_args(argv)

    rel = args.release
    src = sorted(rel.glob("*-src.tar.gz"))
    ffmpeg = sorted(rel.glob("ffmpeg-*.tar.*"))
    lines = [
        f"# webOS Native App Kit {args.version}",
        "",
        f"Built from commit `{args.commit}`. Licence: GPL-3.0-or-later (see `LICENSING.md` in the source).",
        "",
        "## Contents",
        "",
        *package_rows(rel),
        "",
        f"- Corresponding source: `{src[0].name}`" if src else "- Corresponding source: missing",
        f"- FFmpeg source for the bundled libraries (LGPL-2.1 §6): `{ffmpeg[0].name}`" if ffmpeg else
        "- FFmpeg source: not needed (no package bundles FFmpeg)",
        "- `SHA256SUMS` lists every file.",
        {"yes": "- Reproducible: a second build in another directory gave byte-identical packages.",
         "no": "- Reproducible: **no**, a second build differed (see the release log).",
         "unchecked": "- Reproducible: not checked for this release (`tools/release.sh --check-reproducible`)."}[
            args.reproducible],
        "",
        "## Firmware loadability (offline, `tools/fw-check.py`)",
        "",
        *firmware_lines(args.fw_log),
        "",
        "Loadability means every library and symbol the packages need exists in that firmware image; it",
        "is not a test on a TV. Device results follow.",
        "",
        "## Measured on TVs (`docs/compatibility/records.json`)",
        "",
        *compat_sections(args.records),
    ]
    (rel / "RELEASE-NOTES.md").write_text("\n".join(lines).rstrip("\n") + "\n")
    print(f"release-notes: {rel / 'RELEASE-NOTES.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
