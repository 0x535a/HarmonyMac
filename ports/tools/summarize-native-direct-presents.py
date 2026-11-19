#!/usr/bin/env python3
"""Measure DRM presentation intervals for newly published, matched GBM frames of one boot."""
import argparse
from collections import defaultdict, deque
import json
import math
from pathlib import Path
import re
import statistics


def events(path, marker):
    for line in path.read_text(errors="replace").splitlines():
        if marker in line:
            yield dict(re.findall(r"(\w+)=(\d+)", line.split(marker, 1)[1]))


def summarize(boot):
    pending = defaultdict(deque)
    incomplete = 0
    for row in events(boot / "render-service.log", "AURORA_DIRECT_BUFFER_SUBMIT "):
        if not {"device", "inode", "serial", "submit_ns"} <= row.keys():
            incomplete += 1
            continue
        pending[row["device"], row["inode"]].append(int(row["submit_ns"]))
    if not pending:
        raise ValueError("No timestamped GBM publications recorded")

    by_output = defaultdict(list)
    unmatched = ambiguous = 0
    flips = events(boot / "composer.log", "AURORA_DIRECT_BUFFER_FLIP ")
    for row in flips:
        if not {"device", "inode", "timestamp_ns", "monotonic", "output"} <= row.keys():
            incomplete += 1
            continue
        if row.get("monotonic") != "1":
            continue
        stamp = int(row["timestamp_ns"])
        source = pending[row["device"], row["inode"]]
        eligible = []
        while source and source[0] <= stamp:
            eligible.append(source.popleft())
        if len(eligible) == 1:
            by_output[row["output"]].append(stamp)
        elif eligible:
            ambiguous += 1
        else:
            unmatched += 1

    match = re.search(r"AURORA_BOOT_HANDOFF held=0 mono_ms=(\d+) rgb=1 done=1",
                      (boot / "composer.log").read_text(errors="replace"))
    handoff = int(match[1]) * 1_000_000 if match else None

    def intervals(stamps):
        differences = [(b - a) / 1_000_000 for a, b in zip(stamps, stamps[1:]) if b > a]
        ordered = sorted(differences)
        return {
            "matched_frames": len(stamps),
            "first_drm_ns": stamps[0] if stamps else None,
            "last_drm_ns": stamps[-1] if stamps else None,
            "interval_ms": differences,
            "median_ms": statistics.median(ordered) if ordered else None,
            "p95_ms": ordered[math.ceil(0.95 * len(ordered)) - 1] if ordered else None,
        }
    if not by_output:
        raise ValueError("No unique GBM publications matched a monotonic direct flip")
    return {
        "boot_id": boot.name,
        "handoff_ns": handoff,
        "unmatched_present_events": unmatched,
        "ambiguous_present_events": ambiguous,
        "incomplete_trace_records": incomplete,
        "outputs": {key: {"all": intervals(stamps),
                          "after_handoff": intervals([t for t in stamps if t >= handoff]) if handoff else None}
                    for key, stamps in by_output.items()},
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("boot", type=Path, help="Recovered native-diagnostics/<boot-id> directory")
    args = parser.parse_args()
    print(json.dumps(summarize(args.boot), indent=2))
