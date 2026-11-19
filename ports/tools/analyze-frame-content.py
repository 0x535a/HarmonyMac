#!/usr/bin/env python3
"""Classify display flicker from AURORA_SRC_SAMPLE / AURORA_SCAN_SAMPLE frame fingerprints.

Usage: analyze-frame-content.py DIAGNOSTICS_DIR [--out report.json]
DIAGNOSTICS_DIR is a collected native-diagnostics/<boot-id> directory (render-service.log,
composer.log). A single combined serial log may be passed with --log instead.

Findings, in order of specificity:
  stale_on_screen     a scanned frame shows an older RenderService frame than the previous flip
  source_flicker      a source region returns to a value it had 2 frames earlier (A-B-A)
  outside_damage      a source region changed although the frame's swap damage does not cover it
  composition_flicker a scanned region oscillates while the source frames it shows do not
  scanned_write       a buffer's content changed while KMS was scanning it (checked as it leaves)
  gaps                flip intervals above --gap-ms, with nearby clears, pool changes and import errors
"""
import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path
import re

REGION_COLUMNS = REGION_ROWS = 8
GRID_ROWS, SKIP_ROWS = 40, 8
FIELD = re.compile(r"(\w+)=([^ ]+)")


def events(text, marker):
    out = []
    for line in text.splitlines():
        index = line.find(marker)
        if index >= 0:
            out.append(dict(FIELD.findall(line[index + len(marker):])))
    return out


def regions(value):
    return [value[i:i + 4] for i in range(0, len(value), 4)]


def region_rect(index, width, height):
    """Pixel rectangle (x0, y0, x1, y1) covered by a region's sample rows/columns."""
    column, row = index % REGION_COLUMNS, index // REGION_COLUMNS
    usable = height - SKIP_ROWS
    rows_per_region = GRID_ROWS // REGION_ROWS
    first, last = row * rows_per_region, row * rows_per_region + rows_per_region - 1
    y0 = SKIP_ROWS + (2 * first + 1) * usable // (2 * GRID_ROWS)
    y1 = SKIP_ROWS + (2 * last + 1) * usable // (2 * GRID_ROWS) + 1
    return (column * width // REGION_COLUMNS, y0, (column + 1) * width // REGION_COLUMNS, y1)


def intersects(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def oscillations(frames, key):
    """A-B-A per region over consecutive frames. Returns (frame index, region) pairs."""
    found = []
    for i in range(2, len(frames)):
        a, b, c = frames[i - 2][key], frames[i - 1][key], frames[i][key]
        for r in range(min(len(a), len(b), len(c))):
            if a[r] == c[r] != b[r]:
                found.append((i - 1, r))
    return found


def strong(events, times):
    """Oscillations that look like flicker rather than a moving pointer or a single
    toggled widget: >=3 regions in one frame, or one region >=4 times within 1 s."""
    per_frame, per_region = defaultdict(set), defaultdict(list)
    for (index, region), t in zip(events, times):
        per_frame[index].add(region)
        per_region[region].append(t)
    wide = sorted(i for i, regions in per_frame.items() if len(regions) >= 3)
    repeated = {}
    for region, ts in per_region.items():
        ts.sort()
        best = max(sum(1 for u in ts if t <= u < t + 1.0) for t in ts)
        if best >= 4:
            repeated[region] = best
    width_histogram = Counter(min(len(r), 8) for r in per_frame.values())
    return {"wide_frames": len(wide), "repeated_regions": repeated,
            "regions_per_event_frame": dict(sorted(width_histogram.items())),
            "flicker": bool(wide) or bool(repeated)}


def spans(items, times, gap=0.5):
    """Group event times into spans for a readable summary."""
    out = []
    for t in sorted(times):
        if out and t - out[-1][1] <= gap:
            out[-1][1] = t
            out[-1][2] += 1
        else:
            out.append([t, t, 1])
    return [{"from_s": round(a, 3), "to_s": round(b, 3), "events": n} for a, b, n in out]


def analyze(rs_text, composer_text, width, height, gap_ms):
    sources = events(rs_text, "AURORA_SRC_SAMPLE ")
    scans = events(composer_text, "AURORA_SCAN_SAMPLE ")
    pools = events(rs_text, "AURORA_EGL_POOL_CREATE ")
    import_fail = events(rs_text, "AURORA_EGL_IMAGE_IMPORT_FAIL ")
    rebinds = events(rs_text, "AURORA_ASAHI_VA_REBIND ")
    clears = events(composer_text, "AURORA_KMS_CLEAR_RETAINED ") + events(composer_text, "AURORA_KMS_CLEAR_COPY ")
    bind_failures = rs_text.count("DRM_IOCTL_ASAHI_VM_BIND failed")
    submit_failures = rs_text.count("DRM_IOCTL_ASAHI_SUBMIT")
    report = {"display": [width, height], "source_frames": len(sources), "scan_frames": len(scans),
              "pool_creates": len(pools), "image_import_failures": len(import_fail),
              "vm_bind_failures": bind_failures, "va_rebinds": len(rebinds), "submit_failures": submit_failures,
              "clears": len(clears)}

    # Source stream, per producer window, ordered by serial.
    by_window = defaultdict(list)
    for s in sources:
        by_window[s["win"]].append({"serial": int(s["serial"]), "t": int(s["submit_ns"]) / 1e9,
                                    "age": int(s["age"]), "rects": int(s["swap_rects"]),
                                    "box": [int(v) for v in s["swap_box"].split(",")],
                                    "decoded": int(s["decoded"]), "r": regions(s["regions"]),
                                    "cost": int(s["cost_us"])})
    source_by_serial = {}
    flicker_times, outside_times, outside_regions, flicker_regions = [], [], Counter(), Counter()
    flicker_events = []
    for window, frames in by_window.items():
        frames.sort(key=lambda f: f["serial"])
        for f in frames:
            source_by_serial[f["serial"]] = f
        for i, r in oscillations(frames, "r"):
            flicker_times.append(frames[i]["t"])
            flicker_regions[r] += 1
            flicker_events.append((f"{window}:{i}", r))
        for prev, cur in zip(frames, frames[1:]):
            if cur["serial"] != prev["serial"] + 1 or not cur["rects"]:
                continue
            x, y, w, h = cur["box"]
            box = (x, y, x + w, y + h)
            for r, (a, b) in enumerate(zip(prev["r"], cur["r"])):
                if a != b and (not w or not h or not intersects(box, region_rect(r, width, height))):
                    outside_times.append(cur["t"])
                    outside_regions[r] += 1
    report["source"] = {
        "windows": {w: len(f) for w, f in by_window.items()},
        "marker_self_check_failures": sum(f["decoded"] != f["serial"] for fs in by_window.values() for f in fs),
        "age_histogram": dict(Counter(f["age"] for fs in by_window.values() for f in fs)),
        "median_sample_cost_us": sorted(f["cost"] for fs in by_window.values() for f in fs)[len(sources) // 2] if sources else None,
        "flicker_events": len(flicker_times), "flicker_spans": spans(None, flicker_times),
        "flicker_strength": strong(flicker_events, flicker_times),
        "flicker_regions": dict(flicker_regions.most_common(12)),
        "outside_damage_events": len(outside_times), "outside_damage_spans": spans(None, outside_times),
        "outside_damage_regions": dict(outside_regions.most_common(12)),
    }

    # Scanout stream in flip order.
    flips = [{"t": int(s["timestamp_ns"]) / 1e9, "src": int(s["src"]), "fb": int(s["fb"]), "inode": int(s["inode"]),
              "r": regions(s["regions"]), "cost": int(s["cost_us"])} for s in scans]
    flips.sort(key=lambda f: f["t"])
    stale, last = [], -1
    for f in flips:
        if f["src"] >= 0:
            if last >= 0 and f["src"] < last:
                stale.append({"t": round(f["t"], 4), "shown": f["src"], "previous": last, "fb": f["fb"], "inode": f["inode"]})
            last = max(last, f["src"])
    composition, composition_regions, mismatch_regions = [], Counter(), Counter()
    composition_events = []
    for i, r in oscillations(flips, "r"):
        a, b, c = (flips[i - 1]["src"], flips[i]["src"], flips[i + 1]["src"])
        sa, sb, sc = (source_by_serial.get(v) for v in (a, b, c))
        source_oscillates = sa and sb and sc and sa["r"][r] == sc["r"][r] != sb["r"][r]
        if not source_oscillates:
            composition.append(flips[i]["t"])
            composition_regions[r] += 1
            composition_events.append((i, r))
    for f in flips:
        s = source_by_serial.get(f["src"])
        if s:
            for r, (a, b) in enumerate(zip(s["r"], f["r"])):
                if a != b:
                    mismatch_regions[r] += 1
    intervals = [(b["t"] - a["t"]) * 1000 for a, b in zip(flips, flips[1:])]
    report["scan"] = {
        "decoded_fraction": round(sum(f["src"] >= 0 for f in flips) / len(flips), 4) if flips else None,
        "stale_on_screen": len(stale), "stale_examples": stale[:20],
        "repeated_source_flips": sum(1 for a, b in zip(flips, flips[1:]) if b["src"] >= 0 and a["src"] == b["src"]),
        "composition_flicker_events": len(composition), "composition_flicker_spans": spans(None, composition),
        "composition_flicker_strength": strong(composition_events, composition),
        "composition_flicker_regions": dict(composition_regions.most_common(12)),
        "regions_differing_from_source": dict(mismatch_regions.most_common(12)),
        "median_interval_ms": round(sorted(intervals)[len(intervals) // 2], 3) if intervals else None,
        "median_sample_cost_us": sorted(f["cost"] for f in flips)[len(flips) // 2] if flips else None,
    }

    # Gaps and what happened around them.
    markers = ([("clear", int(c.get("timestamp_ns", 0)) / 1e9) for c in clears if c.get("timestamp_ns")] +
               [("pool", int(p["mono_ns"]) / 1e9) for p in pools] +
               [("import_fail", int(e["mono_ns"]) / 1e9) for e in import_fail] +
               [("va_rebind", int(e["mono_ns"]) / 1e9) for e in rebinds])
    gaps = []
    for a, b in zip(flips, flips[1:]):
        if (b["t"] - a["t"]) * 1000 > gap_ms:
            near = sorted({kind for kind, t in markers if a["t"] - 0.2 <= t <= b["t"] + 0.05})
            gaps.append({"from_s": round(a["t"], 3), "ms": round((b["t"] - a["t"]) * 1000, 1), "near": near})
    retired = events(composer_text, "AURORA_SCAN_RETIRE_CHANGED ")
    summaries = events(composer_text, "AURORA_SCAN_RETIRE_SUMMARY ")
    report["scanned_write"] = {
        "changed": len(retired),
        "last_summary": summaries[-1] if summaries else None,
        "spans": spans(None, [int(e["at_ns"]) / 1e9 for e in retired]),
        "examples": retired[:10],
    }
    report["gaps"] = gaps[:60]
    report["gap_count"] = len(gaps)

    verdict = []
    if stale:
        verdict.append("older RenderService frames reached the screen (stale_on_screen)")
    if report["source"]["outside_damage_events"]:
        verdict.append("source content changed outside the declared damage (partial redraw history)")
    if report["source"]["flicker_strength"]["flicker"]:
        verdict.append("the RenderService composition itself oscillates (source_flicker)")
    if retired:
        verdict.append("buffers were modified while on screen (scanned_write)")
    if report["scan"]["composition_flicker_strength"]["flicker"]:
        verdict.append("oscillation appears only after hardware-thread composition/scanout (composition_flicker)")
    report["verdict"] = verdict or ["no content-level flicker detected in the sampled grid"]
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", type=Path, nargs="?")
    parser.add_argument("--log", type=Path, help="single combined log (tests)")
    parser.add_argument("--size", default="", help="WIDTHxHEIGHT; default from AURORA_KMS_OUTPUT or 3024x1964")
    parser.add_argument("--gap-ms", type=float, default=50)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.log:
        rs_text = composer_text = args.log.read_text(errors="replace")
    else:
        rs_text = (args.directory / "render-service.log").read_text(errors="replace")
        composer_text = (args.directory / "composer.log").read_text(errors="replace")
    size = args.size
    if not size:
        mode = re.search(r"AURORA_KMS_OUTPUT .*?mode=(\d+x\d+)", composer_text)
        size = mode[1] if mode else "3024x1964"
    width, height = (int(v) for v in size.split("x"))
    report = analyze(rs_text, composer_text, width, height, args.gap_ms)
    text = json.dumps(report, indent=2)
    if args.out:
        args.out.write_text(text + "\n")
    print(text)
