#!/usr/bin/env python3
"""Host tests for the black-box first-failure attribution tool."""

from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("diag_analyze", ROOT / "tools" / "diag_analyze.py")
assert SPEC and SPEC.loader
DIAG = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DIAG
SPEC.loader.exec_module(DIAG)


def rec(seq: int, up: int, typ: int, dom: int = 3, act: int = 0, code: int = 0, aux: int = 0) -> str:
    return (
        "I (1234) diag: rec seq=%d up=%dms type=%d dom=%d act=%d code=%d aux=%d"
        % (seq, up, typ, dom, act, code, aux)
    )


def analyze(*lines: str) -> dict:
    return DIAG.analyze("\n".join(lines))


class ParseTest(unittest.TestCase):
    def test_parses_records_and_counts_raw_frames(self) -> None:
        records, raw = DIAG.parse_records(
            "\n".join(
                [
                    rec(1, 10, 1),
                    "unrelated boot noise",
                    "diag: raw dir=1 dom=2 len=42",
                    rec(2, 20, 2, act=0, code=0, aux=3),
                ]
            )
        )
        self.assertEqual([(item.seq, item.type) for item in records], [(1, 1), (2, 2)])
        self.assertEqual(raw, 1)
        self.assertEqual(records[1].type_name, "SCAN")

    def test_build_stamp_is_reported(self) -> None:
        report = DIAG.analyze("I (1) diag: black box build: handoff-test-1\n" + rec(1, 10, 2))
        self.assertEqual(report["build"], "handoff-test-1")
        self.assertIn("device build: handoff-test-1", DIAG.render(report))

    def test_repeated_ring_printout_is_deduplicated(self) -> None:
        # A capture that resets twice prints the same ring twice; counts and
        # attribution must not double.
        window = [rec(1, 10, 2), rec(2, 20, 3)]
        report = DIAG.analyze("\n".join(window + window))
        self.assertEqual(report["records"], 2)
        self.assertEqual(report["counts"], {"SCAN": 1, "CONN": 1})

    def test_decode_qual_bit_order(self) -> None:
        decoded = DIAG.decode_qual(1, 0b1111)
        self.assertEqual(decoded["shift"], "P")
        self.assertTrue(decoded["parked_ok"])
        self.assertEqual(decoded["missing"], [])
        partial = DIAG.decode_qual(1, 0b1011)
        self.assertEqual(partial["missing"], ["speed"])
        self.assertFalse(partial["bits_ok"])


class AttributionTest(unittest.TestCase):
    def test_complete_qual_proof_is_not_a_failure(self) -> None:
        report = analyze(
            rec(1, 10, 2),
            rec(2, 20, 3),
            rec(3, 30, 10, act=7),
            rec(4, 40, 12, act=1, code=0xF),
        )
        self.assertIsNone(report["first_failure"])
        self.assertEqual(report["verdict"]["layer"], "none")
        self.assertTrue(report["quals"][0]["parked_ok"])

    def test_missing_speed_bit_attributes_park_proof(self) -> None:
        report = analyze(rec(1, 10, 12, act=1, code=0b1011))
        self.assertEqual(report["first_failure"]["layer"], "park_proof")
        self.assertIn("speed", report["first_failure"]["detail"])

    def test_complete_mask_in_drive_is_a_correct_refusal(self) -> None:
        report = analyze(rec(1, 10, 12, act=2, code=0xF))
        self.assertIsNone(report["first_failure"])
        self.assertFalse(report["quals"][0]["parked_ok"])
        self.assertTrue(report["quals"][0]["bits_ok"])

    def test_response_mtu_fault_is_handled_not_first_failure(self) -> None:
        report = analyze(rec(1, 10, 7, dom=3, act=18, code=25))
        self.assertTrue(report["faults"][0]["handled"])
        self.assertEqual(report["faults"][0]["layer"], "read_plan")
        self.assertIsNone(report["first_failure"])
        self.assertEqual(report["verdict"]["layer"], "none")

    def test_session_fault_attributes_session_layer(self) -> None:
        report = analyze(rec(1, 10, 7, dom=2, act=1, code=5))
        self.assertEqual(report["first_failure"]["layer"], "session")
        self.assertIn("INVALID_SIGNATURE", report["first_failure"]["detail"])

    def test_tx_fail_attributes_send_layer(self) -> None:
        report = analyze(rec(1, 10, 8, dom=2, act=1))
        self.assertEqual(report["first_failure"]["layer"], "send")

    def test_scan_without_connection_attributes_discovery(self) -> None:
        report = analyze(rec(1, 10, 2), rec(2, 12000, 2))
        self.assertIsNone(report["first_failure"])
        self.assertEqual(report["verdict"]["layer"], "discovery")

    def test_send_without_any_reply_attributes_attribution(self) -> None:
        report = analyze(rec(1, 10, 3), rec(2, 100, 10, dom=2, act=1))
        self.assertEqual(report["suspicions"][0]["layer"], "attribution")
        self.assertEqual(report["verdict"]["layer"], "attribution")

    def test_first_failure_is_the_earliest_terminal_incident(self) -> None:
        report = analyze(
            rec(1, 10, 12, act=1, code=0b1110),
            rec(2, 20, 7, dom=2, act=1, code=6),
        )
        self.assertEqual(report["first_failure"]["seq"], 1)
        self.assertEqual(report["first_failure"]["layer"], "park_proof")


class OutputTest(unittest.TestCase):
    SAMPLE = "\n".join(
        [
            rec(1, 10, 2),
            rec(2, 20, 3),
            rec(3, 30, 7, dom=3, act=18, code=25),
            rec(4, 40, 12, act=1, code=0b1110),
        ]
    )

    def test_text_rendering_names_the_layer(self) -> None:
        text = DIAG.render(DIAG.analyze(self.SAMPLE))
        self.assertIn("first failure:", text)
        self.assertIn("layer=park_proof", text)
        self.assertIn("hint:", text)

    def test_report_is_json_serializable(self) -> None:
        payload = json.dumps(DIAG.analyze(self.SAMPLE), ensure_ascii=False)
        self.assertIn("park_proof", payload)

    def test_empty_log_is_not_a_failure(self) -> None:
        report = DIAG.analyze("")
        self.assertEqual(report["records"], 0)
        self.assertIsNone(report["first_failure"])
        self.assertEqual(report["verdict"]["layer"], "none")


if __name__ == "__main__":
    unittest.main()
