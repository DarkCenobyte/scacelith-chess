#!/usr/bin/env python3
"""The README's coverage badge, from gcovr's JSON summary (.github/workflows/coverage.yml).

Usage: python3 -I tools/coverage_badge.py <gcovr summary.json> <badge.json> [<markdown summary>]

Writes a shields.io endpoint badge (https://shields.io/badges/endpoint-badge) with the line
coverage, and appends a short table of the line, function and branch coverage to the Markdown
file (the run's step summary) when one is given.
"""
import json
import sys


def colour(percent):
    for limit, name in ((90, "brightgreen"), (80, "green"), (70, "yellowgreen"), (60, "yellow"), (50, "orange")):
        if percent >= limit:
            return name
    return "red"


def main(argv):
    if len(argv) not in (3, 4):
        sys.exit(__doc__.strip().splitlines()[2])
    with open(argv[1], encoding="utf-8") as f:
        summary = json.load(f)
    lines = float(summary["line_percent"])
    badge = {"schemaVersion": 1, "label": "coverage", "message": f"{lines:.1f}%", "color": colour(lines)}
    with open(argv[2], "w", encoding="utf-8") as f:
        json.dump(badge, f)
        f.write("\n")
    if len(argv) == 4:
        rows = [
            ("Lines", summary["line_covered"], summary["line_total"], summary["line_percent"]),
            ("Functions", summary["function_covered"], summary["function_total"], summary["function_percent"]),
            ("Branches", summary["branch_covered"], summary["branch_total"], summary["branch_percent"]),
        ]
        with open(argv[3], "a", encoding="utf-8") as f:
            f.write("### Test coverage of src/\n\n| | Covered | Total | % |\n|---|---:|---:|---:|\n")
            for name, covered, total, percent in rows:
                f.write(f"| {name} | {covered} | {total} | {float(percent):.1f} |\n")
    print(f"line coverage {lines:.1f}%")


if __name__ == "__main__":
    main(sys.argv)
