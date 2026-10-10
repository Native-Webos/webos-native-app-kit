# SPDX-License-Identifier: GPL-3.0-or-later
"""Access to the webosbrew firmware symbol inventory (webosbrew-toolbox-fw-symbols).

Layout, as written by dev-toolbox-cli `fw-extract`: one directory per firmware image, named
<version>-<otaId>, each with info.json {version, ota_id, release}, index.json {soname-or-file -> json
file} and one <lib>.json {name, package, needed[], symbols[]} per shared library.

The data proves a library/symbol EXISTS in a firmware image. It carries no symbol versions and says
nothing about behaviour.
"""
from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path

ENV_VAR = "WEBOS_FWSYM_DB"


def candidate_dirs() -> list[Path]:
    out = []
    if os.environ.get(ENV_VAR):
        out.append(Path(os.environ[ENV_VAR]))
    out.append(Path("/usr/share/webosbrew/compat-checker/data"))  # the .deb install location
    cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    out.append(cache / "webos-native-app-kit" / "fwsym" / "data")
    out.append(cache / "plxnative" / "fwsym" / "data")  # same upstream package, if fetched by PlxNative
    return out


def find_db(explicit: str | None = None) -> Path | None:
    for d in ([Path(explicit)] if explicit else candidate_dirs()):
        if d.is_dir() and any((d / x / "index.json").is_file() for x in os.listdir(d)):
            return d
    return None


def release_key(release: str) -> tuple[int, ...]:
    return tuple(int(x) if x.isdigit() else 0 for x in release.split("."))


@dataclass
class LibRecord:
    name: str
    needed: list[str]
    symbols: frozenset[str]   # exported names, version suffix removed
    versions: frozenset[str]  # version names this library defines (e.g. GLIBC_2.4)
    file: str


@dataclass
class Firmware:
    path: Path
    version: str
    ota_id: str
    release: str
    index: dict[str, str]
    _cache: dict[str, LibRecord | None] = field(default_factory=dict, repr=False)

    def lib(self, soname: str) -> LibRecord | None:
        if soname in self._cache:
            return self._cache[soname]
        file = self.index.get(soname)
        rec = None
        if file:
            with open(self.path / file, encoding="utf-8") as fh:
                d = json.load(fh)
            names, versions = set(), set()
            for entry in d.get("symbols", []):
                name, _, version = entry.partition("@")
                if version:
                    versions.add(version.lstrip("@"))
                names.add(name)
            rec = LibRecord(name=d.get("name", soname), needed=list(d.get("needed", [])),
                            symbols=frozenset(names), versions=frozenset(versions), file=file)
        self._cache[soname] = rec
        return rec


def load(db: Path) -> list[Firmware]:
    out = []
    for entry in sorted(os.listdir(db)):
        p = db / entry
        try:
            info = json.loads((p / "info.json").read_text(encoding="utf-8"))
            index = json.loads((p / "index.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        out.append(Firmware(path=p, version=info.get("version", "?"), ota_id=info.get("ota_id", "?"),
                            release=info.get("release", "0"), index=index))
    out.sort(key=lambda f: release_key(f.release))
    return out
