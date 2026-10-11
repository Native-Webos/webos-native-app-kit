# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests for tools/check-compat.py (compatibility records against their schema)."""
from __future__ import annotations

import copy
import datetime as dt
import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("check_compat", ROOT / "tools/check-compat.py")
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)

SCHEMA = json.loads((ROOT / "docs/compatibility/schema.json").read_text())
GOOD = {
    "schema": "wnk-compat/1",
    "notes": "",
    "devices": {"lg-test-6.5.3": {"model": "TEST", "webos_release": "6.5.3", "firmware": "03.53.45"}},
    "records": [{"device": "lg-test-6.5.3", "feature": "input.keys.back", "result": "supported",
                 "date": "2026-10-11", "method": "key log", "evidence": "scancode 482", "limitations": ""}],
}
TODAY = dt.date(2026, 10, 11)


def errors_for(mutate) -> list[str]:
    doc = copy.deepcopy(GOOD)
    mutate(doc)
    return cc.check(doc, SCHEMA, TODAY)


class CheckCompatTest(unittest.TestCase):
    def test_repository_records_are_valid(self):
        doc = json.loads((ROOT / "docs/compatibility/records.json").read_text())
        self.assertEqual(cc.check(doc, SCHEMA), [])

    def test_minimal_document_is_valid(self):
        self.assertEqual(errors_for(lambda d: None), [])

    def test_result_must_be_in_enum(self):
        errs = errors_for(lambda d: d["records"][0].update(result="works"))
        self.assertTrue(any("not one of" in e for e in errs), errs)

    def test_missing_field_and_unexpected_key(self):
        def mutate(d):
            del d["records"][0]["evidence"]
            d["records"][0]["verified_by"] = "me"
        errs = errors_for(mutate)
        self.assertTrue(any("missing 'evidence'" in e for e in errs), errs)
        self.assertTrue(any("unexpected key 'verified_by'" in e for e in errs), errs)

    def test_undeclared_device(self):
        errs = errors_for(lambda d: d["records"][0].update(device="lg-other-7.0"))
        self.assertTrue(any("not declared" in e for e in errs), errs)

    def test_future_and_impossible_dates(self):
        self.assertTrue(any("future" in e for e in errors_for(lambda d: d["records"][0].update(date="2027-01-01"))))
        self.assertTrue(any("calendar" in e for e in errors_for(lambda d: d["records"][0].update(date="2026-13-40"))))

    def test_feature_and_firmware_patterns(self):
        self.assertTrue(errors_for(lambda d: d["records"][0].update(feature="Input Keys")))
        self.assertTrue(errors_for(lambda d: d["devices"]["lg-test-6.5.3"].update(firmware="3.53")))

    def test_identifiers_are_rejected(self):
        mac = errors_for(lambda d: d["records"][0].update(evidence="wifi 4C:BC:E9:C5:80:0E"))
        self.assertTrue(any("identifier" in e for e in mac), mac)
        serial = errors_for(lambda d: d["records"][0].update(limitations="serial_number: 111INPT7A032"))
        self.assertTrue(any("identifier" in e for e in serial), serial)


if __name__ == "__main__":
    unittest.main()
