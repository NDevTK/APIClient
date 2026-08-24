#!/usr/bin/env python3
"""Render live-matrix.sh's per-run rows as one table, and state the SPREAD per site.

It never averages and it never fills a hole: a counter the engine did not report prints
`-` and a counter it reported as zero prints `0`, because an absent count and a zero
count are different facts (CLAUDE.md §Testing). A crashed run carries no counters at all
by bridge.js's own rule, so its whole row is dashes with the verdict left to say why.

    python3 testing/live-table.py <matrix-output-file>
"""
import json
import sys
from collections import OrderedDict

KEYS = ["endpoints", "sinks", "candidates", "flows", "switches", "jobsQueued", "jobsRun", "park"]


def spread(vals):
    """A RANGE, never a mean — and `-` when nothing was ever reported, which is not zero."""
    v = [x for x in vals if isinstance(x, int)]
    if not v:
        return "-"
    return str(v[0]) if min(v) == max(v) else "%d–%d" % (min(v), max(v))


def main():
    rows, url = [], None
    for ln in open(sys.argv[1]):
        if ln.startswith("###"):
            url = ln.split("url=")[1].strip()
        elif ln.startswith("{"):
            try:
                r = json.loads(ln)
            except ValueError:
                continue
            if "counters" not in r:      # live-run.js's own per-site spread line; this file re-derives it
                continue
            c = r["counters"][0] if r["counters"] else {}
            rows.append((url, r["verdict"], [c.get(k) for k in KEYS], r.get("storeEndpointsDelta")))

    hdr = "%-40s %-26s" % ("url", "verdict") + "".join("%9s" % k[:9] for k in KEYS) + "%6s" % "dEp"
    print(hdr)
    print("-" * len(hdr))
    for u, verdict, cs, d in rows:
        print("%-40s %-26s" % (u[:40], verdict[:26])
              + "".join("%9s" % ("-" if c is None else c) for c in cs)
              + "%6s" % ("-" if d is None else d))

    per = OrderedDict()
    for u, verdict, cs, d in rows:
        per.setdefault(u, []).append((verdict, cs))
    print("\n--- spread per site (a range, never a mean; n = runs, each its own browser) ---")
    for u, rs in per.items():
        cols = " ".join("%s=%s" % (k, spread([c[i] for _, c in rs])) for i, k in enumerate(KEYS))
        print("%-40s n=%d %s" % (u[:40], len(rs), cols))
        print("%-40s      verdicts: %s" % ("", ", ".join(v for v, _ in rs)))


main()
