# SPDX-License-Identifier: GPL-3.0-or-later
"""Read .ipk archives (ar container with control.tar.gz and data.tar.gz). Stdlib only."""
from __future__ import annotations

import io
import json
import posixpath
import tarfile
from dataclasses import dataclass, field
from pathlib import Path


def read_ar(blob: bytes) -> list[tuple[str, bytes]]:
    """Members of a common-format ar archive as (name, bytes). A GNU-style trailing '/' is kept,
    so callers can detect it."""
    if not blob.startswith(b"!<arch>\n"):
        raise ValueError("not an ar archive")
    pos, members = 8, []
    while pos < len(blob):
        header = blob[pos:pos + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise ValueError(f"malformed ar header at offset {pos}")
        name = header[:16].decode("ascii").rstrip(" ")
        size = int(header[48:58].decode("ascii").strip())
        data = blob[pos + 60:pos + 60 + size]
        if len(data) != size:
            raise ValueError("truncated ar member")
        members.append((name, data))
        pos += 60 + size + (size % 2)
    return members


@dataclass
class AppPayload:
    app_id: str
    appinfo: dict
    root: str  # "usr/palm/applications/<id>/"
    files: dict[str, bytes] = field(default_factory=dict)  # normalised archive path -> bytes
    modes: dict[str, int] = field(default_factory=dict)

    @property
    def main_path(self) -> str:
        return posixpath.normpath(self.root + self.appinfo.get("main", ""))


def read_payload(ipk: Path) -> AppPayload:
    members = dict(read_ar(ipk.read_bytes()))
    data = members.get("data.tar.gz")
    if data is None:
        raise ValueError("no data.tar.gz member")
    files: dict[str, bytes] = {}
    modes: dict[str, int] = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for m in tf.getmembers():
            if m.isfile():
                name = posixpath.normpath(m.name[2:] if m.name.startswith("./") else m.name)
                files[name] = tf.extractfile(m).read()
                modes[name] = m.mode & 0o7777
    appinfo_paths = [p for p in files if p.startswith("usr/palm/applications/") and p.endswith("/appinfo.json")
                     and p.count("/") == 4]
    if len(appinfo_paths) != 1:
        raise ValueError(f"expected exactly one application appinfo.json, found {appinfo_paths}")
    appinfo = json.loads(files[appinfo_paths[0]])
    app_id = appinfo_paths[0].split("/")[3]
    return AppPayload(app_id=app_id, appinfo=appinfo, root=f"usr/palm/applications/{app_id}/",
                      files=files, modes=modes)
