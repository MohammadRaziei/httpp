"""Summarise an Autobahn report as Markdown and fail (exit 1) if a case FAILED.
Usage: python check_report.py reports/index.json [out.md]"""

import json
import os
import sys

index = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else None
base = os.path.dirname(index)
report = json.load(open(index, encoding="utf-8"))


def order(case):
    return [int(p) if p.isdigit() else 0 for p in case.split(".")]


counts, notable = {}, []
for agent, cases in report.items():
    for case, result in sorted(cases.items(), key=lambda kv: order(kv[0])):
        for key in ("behavior", "behaviorClose"):
            counts[result[key]] = counts.get(result[key], 0) + 1
        if result["behavior"] != "OK" or result["behaviorClose"] != "OK":
            notable.append((case, result))


def clip(text, n=600):
    text = str(text)
    return text if len(text) <= n else text[:n] + " ..."


failed = [n for n in notable if "FAILED" in (n[1]["behavior"], n[1]["behaviorClose"])]
lines = ["# Autobahn report", "", "Counts (behavior and behaviorClose together): " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())), ""]
lines.append(f"FAILED cases: {len(failed)}" if failed else "No FAILED case.")
lines.append("")
for case, result in notable:
    lines.append(f"## {case}: behavior {result['behavior']}, close {result['behaviorClose']}")
    try:
        detail = json.load(open(os.path.join(base, result["reportfile"]), encoding="utf-8"))
        for key in ("description", "expected", "observed", "result", "resultClose"):
            if key in detail:
                lines.append(f"- {key}: {clip(detail[key])}")
    except (OSError, KeyError, ValueError):
        pass
    lines.append("")
text = "\n".join(lines)
if out_path:
    open(out_path, "w", encoding="utf-8").write(text)
print(text[:4000])
sys.exit(1 if failed else 0)
