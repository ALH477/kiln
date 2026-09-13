#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""report_check.py — hold a validator's --json output to tools/schema/report.schema.json.

    report_check.py <report.json>        exit 0 if it conforms, 1 with the reasons if not
    report_check.py --selftest           prove every rule here rejects what it should

Written out by hand rather than with a JSON Schema library: the studio runs on
python3Minimal, and the schema is small enough that its rules fit on one screen.
The one rule a schema cannot state — `ok` is false exactly when there is an
error — is the one that matters most, because a report that says ok while
listing a failure is a dashboard that shows green over red.
"""

import json
import re
import sys

CODE = re.compile(r"^[A-Z][A-Z0-9_]*$")


def problems(r):
    out = []
    if not isinstance(r, dict):
        return ["the report is not a JSON object"]
    for key, kind in (("tool", str), ("version", int), ("ok", bool), ("errors", list),
                      ("notes", list), ("metrics", dict)):
        if key not in r:
            out.append(f"missing '{key}'")
        elif not isinstance(r[key], kind) or (kind is int and isinstance(r[key], bool)):
            out.append(f"'{key}' is {type(r[key]).__name__}, want {kind.__name__}")
    if isinstance(r.get("tool"), str) and not r["tool"]:
        out.append("'tool' is empty")
    if isinstance(r.get("version"), int) and not isinstance(r.get("version"), bool) and r["version"] < 1:
        out.append("'version' must be at least 1")
    for key in ("errors", "notes"):
        for i, f in enumerate(r.get(key) or [] if isinstance(r.get(key), list) else []):
            where = f"{key}[{i}]"
            if not isinstance(f, dict):
                out.append(f"{where} is not an object")
                continue
            if not isinstance(f.get("code"), str) or not CODE.match(f["code"]):
                out.append(f"{where}.code {f.get('code')!r} is not an UPPER_SNAKE code")
            if not isinstance(f.get("msg"), str) or not f["msg"]:
                out.append(f"{where}.msg is missing or empty")
            if "where" in f and not isinstance(f["where"], str):
                out.append(f"{where}.where is not a string")
    if isinstance(r.get("ok"), bool) and isinstance(r.get("errors"), list):
        if r["ok"] != (len(r["errors"]) == 0):
            out.append(f"'ok' is {r['ok']} with {len(r['errors'])} error(s): ok must be true exactly when errors is empty")
    return out


def selftest():
    good = {"tool": "t", "version": 1, "ok": False, "errors": [{"code": "BAD", "msg": "m", "where": "w"}],
            "notes": [], "metrics": {}}
    cases = {
        "a conforming report": (good, False),
        "green over red": ({**good, "ok": True}, True),
        "red with nothing wrong": ({**good, "errors": []}, True),
        "missing metrics": ({k: v for k, v in good.items() if k != "metrics"}, True),
        "a lowercase code": ({**good, "errors": [{"code": "bad", "msg": "m"}]}, True),
        "an empty message": ({**good, "errors": [{"code": "BAD", "msg": ""}]}, True),
        "a boolean version": ({**good, "version": True}, True),
        "not an object": ([], True),
    }
    fail = 0
    for label, (report, should_fail) in cases.items():
        got = bool(problems(report))
        ok = got == should_fail
        print(f"  {'ok  ' if ok else 'FAIL'} {label}: {'rejected' if got else 'accepted'}")
        fail += 0 if ok else 1
    print("report_check selftest " + ("FAILED" if fail else "passed"))
    return 1 if fail else 0


def main(argv):
    if argv == ["--selftest"]:
        return selftest()
    if len(argv) != 1:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    try:
        report = json.load(open(argv[0]) if argv[0] != "-" else sys.stdin)
    except ValueError as e:
        print(f"report_check: not JSON: {e}")
        return 1
    found = problems(report)
    for p in found:
        print(f"report_check: {p}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
