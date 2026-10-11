#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check docs/compatibility/records.json against docs/compatibility/schema.json.

    tools/check-compat.py [RECORDS] [--schema SCHEMA]

The schema is the source of truth; this tool applies the subset of JSON Schema it uses (type,
required, properties, additionalProperties, propertyNames, enum, pattern, minLength, items) without
third-party packages, then checks what a schema cannot: every record's device is declared, no date
lies in the future, and no string carries a device identifier (MAC address, serial number, nduid).
Exit status 0 when clean, 1 with one line per problem otherwise.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TYPES = {"object": dict, "array": list, "string": str, "integer": int, "number": (int, float), "boolean": bool}
MAC = re.compile(r"\b[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}\b")
IDENTIFIER_WORDS = re.compile(r"\b(serial_number|nduid|wifi_addr|wired_addr)\b\s*[=:]\s*\S", re.I)


def validate(value, schema: dict, path: str, errors: list[str]) -> None:
    t = schema.get("type")
    if t is not None:
        py = TYPES[t]
        if not isinstance(value, py) or (t == "integer" and isinstance(value, bool)):
            errors.append(f"{path}: expected {t}")
            return
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path}: {value!r} not one of {schema['enum']}")
    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0):
            errors.append(f"{path}: empty")
        if "pattern" in schema and not re.search(schema["pattern"], value):
            errors.append(f"{path}: {value!r} does not match {schema['pattern']}")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{path}: missing {key!r}")
        props = schema.get("properties", {})
        names = schema.get("propertyNames")
        extra = schema.get("additionalProperties", True)
        for key, sub in value.items():
            if names is not None:
                validate(key, names, f"{path}.{key} (name)", errors)
            if key in props:
                validate(sub, props[key], f"{path}.{key}", errors)
            elif extra is False:
                errors.append(f"{path}: unexpected key {key!r}")
            elif isinstance(extra, dict):
                validate(sub, extra, f"{path}.{key}", errors)
    if isinstance(value, list) and "items" in schema:
        for i, item in enumerate(value):
            validate(item, schema["items"], f"{path}[{i}]", errors)


def strings(value, path: str):
    if isinstance(value, str):
        yield path, value
    elif isinstance(value, dict):
        for k, v in value.items():
            yield from strings(v, f"{path}.{k}")
    elif isinstance(value, list):
        for i, v in enumerate(value):
            yield from strings(v, f"{path}[{i}]")


def check(records: dict, schema: dict, today: dt.date | None = None) -> list[str]:
    errors: list[str] = []
    validate(records, schema, "$", errors)
    if errors:
        return errors
    today = today or dt.date.today()
    for i, r in enumerate(records["records"]):
        where = f"$.records[{i}] ({r['device']} {r['feature']})"
        if r["device"] not in records["devices"]:
            errors.append(f"{where}: device {r['device']!r} is not declared in devices")
        try:
            if dt.date.fromisoformat(r["date"]) > today:
                errors.append(f"{where}: date {r['date']} is in the future")
        except ValueError:
            errors.append(f"{where}: date {r['date']!r} is not a calendar date")
    for path, s in strings(records, "$"):
        if MAC.search(s) or IDENTIFIER_WORDS.search(s):
            errors.append(f"{path}: looks like a device identifier; records must not contain one")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("records", nargs="?", type=Path, default=ROOT / "docs/compatibility/records.json")
    ap.add_argument("--schema", type=Path, default=ROOT / "docs/compatibility/schema.json")
    args = ap.parse_args()
    errors = check(json.loads(args.records.read_text()), json.loads(args.schema.read_text()))
    for e in errors:
        print(f"check-compat: {e}")
    if not errors:
        n = len(json.loads(args.records.read_text())["records"])
        print(f"check-compat: {args.records.name}: {n} records ok")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
