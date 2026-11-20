#!/usr/bin/env python3
"""Attribute the first failing layer from a captured firmware black box.

The device writes a persistent ring in the ``bblog`` partition and prints it on
the next boot over USB Serial/JTAG as ``diag: rec ...`` lines. ``diag_capture.py``
captures that text; this module turns it into an evidence-based verdict instead
of a guess.

Attribution rules (stable branch, 2026-11-18):

* ``type=12 QUAL`` -- the vehicle's qualification reply. ``act`` is the shift
  (0 absent, 1 P, 2 D, 3 R, 4 N, 5 invalid) and ``code`` is a bit mask:
  bit0 live/correlated, bit1 authenticated, bit2 speed field present,
  bit3 sample present. A park proof needs all four bits and an explicit P.
  A complete mask with shift D/R/N is the vehicle correctly refusing the
  action, not a firmware failure.
* ``type=7 FAULT`` -- classified by the vehicle's MessageFault code. Fault 24
  (request MTU) / 25 (response MTU) point at read planning; key/counter/epoch/
  signature/encryption faults point at the session layer; busy/timeout/TTL
  point at timing; the remaining codes are deterministic vehicle policies.
* ``type=6 SESSION_FAIL`` -- handshake reply was not authenticated/correlated.
* ``type=8 TX_FAIL`` -- the BLE write itself failed (MTU/link), so nothing is
  known about the vehicle's behaviour.

A fault 25 during a multi-field read is expected in this firmware: it is
handled by skipping that VehicleData section and reading the next one. It is
reported but does not become the "first failure" unless nothing else succeeds.

This tool is read-only: it never opens a serial port and never touches NVS.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from typing import Any, Iterable

TYPE_NAMES = {
    1: "BOOT",
    2: "SCAN",
    3: "CONN",
    4: "DISC",
    5: "SESSION",
    6: "SESSION_FAIL",
    7: "FAULT",
    8: "TX_FAIL",
    9: "RX_PLAINTEXT",
    10: "SEND",
    11: "HINT",
    12: "QUAL",
    13: "MTU",
}
TYPE_BOOT = 1
TYPE_SCAN = 2
TYPE_CONN = 3
TYPE_DISC = 4
TYPE_SESSION = 5
TYPE_SESSION_FAIL = 6
TYPE_FAULT = 7
TYPE_TX_FAIL = 8
TYPE_RX_PLAINTEXT = 9
TYPE_SEND = 10
TYPE_HINT = 11
TYPE_QUAL = 12
TYPE_MTU = 13

FAULT_NAMES = {
    0: "NONE",
    1: "BUSY",
    2: "TIMEOUT",
    3: "UNKNOWN_KEY",
    4: "INACTIVE_KEY",
    5: "INVALID_SIGNATURE",
    6: "INVALID_COUNTER",
    7: "INSUFFICIENT_PRIVILEGES",
    8: "INVALID_DOMAINS",
    9: "INVALID_COMMAND",
    10: "DECODING",
    11: "INTERNAL",
    12: "WRONG_VIN",
    13: "BAD_PARAMETER",
    14: "KEYCHAIN_FULL",
    15: "INCORRECT_EPOCH",
    16: "IV_LENGTH",
    17: "TIME_EXPIRED",
    18: "NOT_PROVISIONED",
    19: "HASH",
    20: "TTL_TOO_LONG",
    21: "REMOTE_ACCESS_DISABLED",
    22: "REMOTE_SERVICE_DISABLED",
    23: "ACCOUNT_REQUIRED",
    24: "REQUEST_MTU",
    25: "RESPONSE_MTU_EXCEEDED",
    26: "REPEATED_COUNTER",
    27: "INVALID_KEY_HANDLE",
    28: "REQUIRES_RESPONSE_ENCRYPTION",
}

# Read planning is ours to fix; session/key faults are ours to diagnose but the
# vehicle decides; timing faults are link behaviour; the rest are vehicle policy.
FAULT_LAYERS = {
    24: "read_plan",
    25: "read_plan",
    3: "session",
    4: "session",
    5: "session",
    6: "session",
    7: "session",
    12: "session",
    15: "session",
    26: "session",
    27: "session",
    28: "session",
    1: "timing",
    2: "timing",
    11: "timing",
    17: "timing",
    20: "timing",
}

# Fault 24/25 are handled by the request planner; they are not terminal.
HANDLED_FAULTS = {24, 25}

DOMAIN_NAMES = {0: "broadcast", 2: "VCSEC", 3: "Infotainment"}

# action ids used by FAULT/TX_FAIL/RX_PLAINTEXT records (pk_action_t).
ACTION_NAMES = {
    0: "NONE",
    1: "LOCK",
    2: "UNLOCK",
    3: "FRUNK_OPEN",
    4: "TRUNK_MOVE",
    5: "CLIMATE_ON",
    6: "PRECONDITION_MAX_ON",
    7: "CHARGE_PORT_OPEN",
    8: "CHARGE_PORT_CLOSE",
    9: "WINDOW_VENT",
    10: "FLASH",
    11: "HONK",
    12: "SET_SENTRY",
    13: "SET_SEAT_HEAT",
    14: "SET_STEERING_HEAT",
    15: "GET_QUALIFICATION",
    16: "VCSEC_WAKE_ONCE",
    17: "VCSEC_GET_STATUS_ONCE",
    18: "STATUS_READ",
    19: "ADDKEY",
    20: "SELECT",
    21: "RETURN_MAIN",
    22: "RECONNECT",
    23: "NO_TARGET",
    24: "PAIR_VERIFY_VCSEC",
    25: "PAIR_VERIFY_INFOTAINMENT",
}

SHIFT_NAMES = {0: "none", 1: "P", 2: "D", 3: "R", 4: "N", 5: "invalid"}

QUAL_BITS = (
    ("live", 0x1),
    ("auth", 0x2),
    ("speed", 0x4),
    ("sample", 0x8),
)

LAYER_HINTS = {
    "discovery": "No connection at all: check the VIN-derived BLE name, advertising window and scan timing.",
    "connect": "Connect failed or never reached a session: check RSSI, connection parameters and vehicle occupancy.",
    "session": "Handshake rejected or invalidated: compare SESSION/SESSION_FAIL/FAULT codes with the session layer.",
    "read_plan": "Read planning: one VehicleData section per request; fault 24/25 must not be retried as-is.",
    "send": "BLE write failed: nothing is known about the vehicle; check MTU, link and GATT readiness.",
    "attribution": "A request was sent but no attributable reply was recorded: check routing address, request UUID and plaintext acceptance.",
    "park_proof": "Park proof bits missing: live/correlated, authenticated, speed present, sample present.",
    "vehicle_rejected": "The vehicle returned a deterministic policy/parameter error; this is not a firmware layer.",
    "timing": "Busy/timeout/TTL: link timing or deadline, not a status or command decision.",
}

REC_RE = re.compile(
    r"diag: rec seq=(\d+) up=(\d+)ms type=(\d+) dom=(\d+) act=(\d+) code=(-?\d+) aux=(-?\d+)"
)
RAW_RE = re.compile(r"diag: raw ")
BUILD_RE = re.compile(r"black box build:\s*(\S+)")


@dataclass
class Record:
    seq: int
    uptime_ms: int
    type: int
    domain: int
    action: int
    code: int
    aux: int

    @property
    def type_name(self) -> str:
        return TYPE_NAMES.get(self.type, str(self.type))

    @property
    def domain_name(self) -> str:
        return DOMAIN_NAMES.get(self.domain, str(self.domain))

    @property
    def action_name(self) -> str:
        return ACTION_NAMES.get(self.action, str(self.action))


def parse_records(text: str) -> tuple[list[Record], int]:
    """Return the ring records in print order plus the raw-frame count."""

    records: list[Record] = []
    seen: set[tuple[int, ...]] = set()
    raw = 0
    for line in text.splitlines():
        match = REC_RE.search(line)
        if match:
            record = Record(*(int(value) for value in match.groups()))
            # A capture can reset the chip twice and print the same ring twice;
            # deduplicate exact repeats so counts and attribution stay honest.
            key = (record.seq, record.uptime_ms, record.type, record.domain,
                   record.action, record.code, record.aux)
            if key not in seen:
                seen.add(key)
                records.append(record)
        elif RAW_RE.search(line):
            raw += 1
    return records, raw


def decode_qual(action: int, code: int) -> dict[str, Any]:
    """Decode one type=12 QUAL record: action is shift, code is the bit mask."""

    bits = {name: bool(code & mask) for name, mask in QUAL_BITS}
    missing = [name for name, mask in QUAL_BITS if not code & mask]
    shift = SHIFT_NAMES.get(action, str(action))
    return {
        "shift": shift,
        "bits": bits,
        "missing": missing,
        "bits_ok": not missing,
        # All four bits plus an explicit P: the only state that may unlock a
        # park action. A complete mask with D/R/N is a correct refusal.
        "parked_ok": not missing and action == 1,
    }


def classify_fault(code: int) -> str:
    return FAULT_LAYERS.get(code, "vehicle_rejected")


def _incidents(records: Iterable[Record]) -> list[dict[str, Any]]:
    incidents: list[dict[str, Any]] = []
    for record in records:
        if record.type == TYPE_SESSION_FAIL:
            incidents.append(
                {
                    "seq": record.seq,
                    "uptime_ms": record.uptime_ms,
                    "layer": "session",
                    "detail": "SessionInfo reply was not authenticated/correlated",
                    "handled": False,
                }
            )
        elif record.type == TYPE_TX_FAIL:
            incidents.append(
                {
                    "seq": record.seq,
                    "uptime_ms": record.uptime_ms,
                    "layer": "send",
                    "detail": f"{record.action_name} write failed before the vehicle saw it",
                    "handled": False,
                }
            )
        elif record.type == TYPE_FAULT:
            name = FAULT_NAMES.get(record.code, str(record.code))
            incidents.append(
                {
                    "seq": record.seq,
                    "uptime_ms": record.uptime_ms,
                    "layer": classify_fault(record.code),
                    "detail": f"{record.action_name} fault {record.code} ({name})",
                    "handled": record.code in HANDLED_FAULTS,
                }
            )
        elif record.type == TYPE_QUAL:
            qual = decode_qual(record.action, record.code)
            if not qual["bits_ok"]:
                incidents.append(
                    {
                        "seq": record.seq,
                        "uptime_ms": record.uptime_ms,
                        "layer": "park_proof",
                        "detail": "missing " + ", ".join(qual["missing"]),
                        "handled": False,
                    }
                )
    return incidents


def _suspicions(records: list[Record]) -> list[dict[str, Any]]:
    """Non-terminal hints for cases with no explicit fault record."""

    counts: dict[int, int] = {}
    for record in records:
        counts[record.type] = counts.get(record.type, 0) + 1

    hints: list[dict[str, Any]] = []
    if counts.get(TYPE_SCAN) and not counts.get(TYPE_CONN):
        hints.append({"layer": "discovery", "detail": "scans recorded, no connection"})
    if counts.get(TYPE_CONN) and not counts.get(TYPE_SESSION) and not counts.get(TYPE_SEND):
        hints.append(
            {"layer": "connect", "detail": "connected but no session and no signed request"}
        )

    sends = [record for record in records if record.type == TYPE_SEND]
    replies = [
        record
        for record in records
        if record.type in (TYPE_RX_PLAINTEXT, TYPE_HINT, TYPE_FAULT, TYPE_QUAL, TYPE_SESSION)
    ]
    if sends and not replies:
        hints.append(
            {
                "layer": "attribution",
                "detail": "requests were sent but the ring holds no reply at all",
            }
        )
    return hints


def analyze(text: str) -> dict[str, Any]:
    records, raw = parse_records(text)
    build_match = BUILD_RE.search(text)
    counts: dict[str, int] = {}
    for record in records:
        counts[record.type_name] = counts.get(record.type_name, 0) + 1

    faults = [
        {
            "seq": record.seq,
            "uptime_ms": record.uptime_ms,
            "domain": record.domain_name,
            "action": record.action_name,
            "code": record.code,
            "name": FAULT_NAMES.get(record.code, str(record.code)),
            "layer": classify_fault(record.code),
            "handled": record.code in HANDLED_FAULTS,
        }
        for record in records
        if record.type == TYPE_FAULT
    ]

    quals = [
        {"seq": record.seq, "uptime_ms": record.uptime_ms, "domain": record.domain_name,
         "code": record.code, **decode_qual(record.action, record.code)}
        for record in records
        if record.type == TYPE_QUAL
    ]

    incidents = _incidents(records)
    terminal = [incident for incident in incidents if not incident["handled"]]
    first = terminal[0] if terminal else None
    hints = _suspicions(records)

    if first:
        verdict = first["layer"]
        reason = first["detail"]
    elif hints:
        verdict = hints[0]["layer"]
        reason = hints[0]["detail"]
    elif any(fault["handled"] for fault in faults):
        verdict = "none"
        reason = "only handled fault 24/25 records"
    else:
        verdict = "none"
        reason = "no failure recorded in this window"

    return {
        "build": build_match.group(1) if build_match else "",
        "records": len(records),
        "raw_frames": raw,
        "counts": counts,
        "faults": faults,
        "quals": quals,
        "incidents": incidents,
        "suspicions": hints,
        "first_failure": first,
        "verdict": {"layer": verdict, "reason": reason, "hint": LAYER_HINTS.get(verdict, "")},
    }


def render(report: dict[str, Any]) -> str:
    lines: list[str] = []
    if report.get("build"):
        lines.append("device build: %s" % report["build"])
    lines.append("Black box: %d records, %d raw frames" % (report["records"], report["raw_frames"]))
    if report["counts"]:
        lines.append(
            "types: "
            + ", ".join("%s=%d" % (name, count) for name, count in sorted(report["counts"].items()))
        )

    if report["faults"]:
        lines.append("faults:")
        for fault in report["faults"]:
            suffix = " (handled by read planning)" if fault["handled"] else ""
            lines.append(
                "  seq=%d up=%dms %s %s fault=%d(%s) layer=%s%s"
                % (
                    fault["seq"],
                    fault["uptime_ms"],
                    fault["domain"],
                    fault["action"],
                    fault["code"],
                    fault["name"],
                    fault["layer"],
                    suffix,
                )
            )

    if report["quals"]:
        lines.append("qualification replies:")
        for qual in report["quals"]:
            bits = ",".join(name for name, present in qual["bits"].items() if present) or "-"
            if qual["parked_ok"]:
                state = "proof ok (P)"
            elif qual["bits_ok"]:
                state = "refused: shift=%s (correct for a moving car)" % qual["shift"]
            else:
                state = "NO PROOF: missing %s" % ",".join(qual["missing"])
            lines.append("  seq=%d shift=%s bits=%s %s" % (qual["seq"], qual["shift"], bits, state))

    first = report["first_failure"]
    if first:
        lines.append(
            "first failure: seq=%d up=%dms layer=%s -- %s"
            % (first["seq"], first["uptime_ms"], first["layer"], first["detail"])
        )
    else:
        lines.append("first failure: none recorded")
    lines.append("verdict: %s -- %s" % (report["verdict"]["layer"], report["verdict"]["reason"]))
    if report["verdict"]["hint"]:
        lines.append("hint: %s" % report["verdict"]["hint"])

    if report["suspicions"]:
        lines.append("suspicions:")
        for hint in report["suspicions"]:
            lines.append("  layer=%s -- %s" % (hint["layer"], hint["detail"]))
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", nargs="?", help="captured diag text; default: stdin")
    parser.add_argument("--json", action="store_true", help="print JSON instead of text")
    parser.add_argument("--out", help="also write the JSON report to this path")
    args = parser.parse_args(argv)

    if args.log:
        with open(args.log, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    else:
        text = sys.stdin.read()

    report = analyze(text)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(render(report))

    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
