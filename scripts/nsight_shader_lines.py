"""Summarize an Nsight Graphics Shader Profiler source export (per-line CSV) for register and stall hot spots.

Export from the Shader Profiler's source view (one row per source line, a header row per file). Usage:

    python scripts/nsight_shader_lines.py D:/shader_registers.csv [--threshold 80] [--top 30]

Prints, per file, the highest live-register count and the share of samples; the regions whose live registers exceed the
threshold (the lines to look at for an occupancy step); the share of samples by live-register band (a plateau across a
whole phase means many loads in flight, not one long live range); and the lines with the most samples with their top
stall reason. Samples are a line's own; code inlined from included files (which the export does not list) counts only in
the call site's Total Samples, so the last section ranks lines by that (a call site's inclusive share; nested call sites
overlap, so these shares do not sum to 100%).
"""

import argparse
import collections
import csv


def parse(path):
    rows = []
    current_file = None
    with open(path, newline="", encoding="utf-8-sig") as f:
        reader = csv.reader(f)
        header = next(reader)
        col = {name: i for i, name in enumerate(header)}

        def number(row, name):
            i = col.get(name)
            if i is None or i >= len(row):
                return 0.0
            try:
                return float(row[i])
            except ValueError:
                return 0.0

        for row in reader:
            if not row:
                continue
            if row[0] == "" and len(row) > 1 and row[1]:
                if current_file is not None:
                    break  # Later sections are the DXIL and per-instruction views, not source files.
                current_file = row[1]
                continue
            try:
                line = int(row[0])
            except ValueError:
                continue
            stall_index = col.get("Top Stall #1 (Type)")
            rows.append(
                {
                    "file": current_file,
                    "line": line,
                    "source": row[1].strip() if len(row) > 1 else "",
                    "samples": number(row, "Samples"),
                    "total": number(row, "Total Samples"),
                    "stall": row[stall_index] if stall_index is not None and stall_index < len(row) else "",
                    "registers": number(row, "Live Registers"),
                }
            )
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv")
    parser.add_argument("--threshold", type=float, default=80.0, help="live registers above which a line is reported")
    parser.add_argument("--gap", type=int, default=6, help="source lines between two hot lines that still join a region")
    parser.add_argument("--top", type=int, default=30, help="lines to list by samples")
    args = parser.parse_args()

    rows = parse(args.csv)
    total = sum(r["samples"] for r in rows) or 1.0
    files = collections.OrderedDict()
    for r in rows:
        files.setdefault(r["file"], []).append(r)

    print(f"total samples {total:.0f}; lines with code {sum(1 for r in rows if r['registers'] > 0)}")
    print("\n== per file: max live registers, lines over the threshold, samples")
    for name, lines in files.items():
        code = [r for r in lines if r["registers"] > 0]
        if not code:
            continue
        samples = sum(r["samples"] for r in lines)
        over = sum(1 for r in code if r["registers"] > args.threshold)
        print(f"{name:45s} max {max(r['registers'] for r in code):4.0f}  >{args.threshold:.0f}: {over:4d}  "
              f"samples {samples:7.0f} ({100 * samples / total:4.1f}%)")

    print(f"\n== regions over {args.threshold:.0f} live registers")
    for name, lines in files.items():
        region = []
        last = None

        def flush():
            if not region:
                return
            peak = max(region, key=lambda r: r["registers"])
            samples = sum(r["samples"] for r in region)
            print(f"{name}:{region[0]['line']}-{region[-1]['line']}  max {peak['registers']:.0f} at {peak['line']} "
                  f"'{peak['source'][:70]}'  lines {len(region)}  samples {samples:.0f}")

        for r in lines:
            if r["registers"] > args.threshold:
                if last is not None and r["line"] - last > args.gap:
                    flush()
                    region = []
                region.append(r)
                last = r["line"]
        flush()

    print("\n== share of samples by live-register band")
    bands = collections.Counter()
    for r in rows:
        if r["registers"] > 0:
            bands[int(r["registers"]) // 8 * 8] += r["samples"]
    banded = sum(bands.values()) or 1.0
    for band in sorted(bands):
        print(f"{band:3d}-{band + 7:3d}: {100 * bands[band] / banded:5.1f}%")

    print(f"\n== top {args.top} lines by samples")
    for r in sorted(rows, key=lambda r: -r["samples"])[: args.top]:
        print(f"{r['samples']:7.0f} {100 * r['samples'] / total:4.1f}%  regs {r['registers']:3.0f}  {r['stall']:16s} "
              f"{r['file']}:{r['line']}  {r['source'][:80]}")

    # The entry point's lines (from the last line holding "[numthreads" on) never nest in each other, so their totals sum to
    # the shader's samples with every inlined callee counted once.
    if any(r["total"] > r["samples"] for r in rows):
        entry = max((r["line"] for r in rows if "[numthreads" in r["source"]), default=0)
        whole = sum(r["total"] for r in rows if r["line"] >= entry) or 1.0
        print(f"\n== top {args.top} lines by total samples (with inlined callees), share of {whole:.0f} "
              f"(the entry point's lines; nested call sites overlap)")
        for r in sorted(rows, key=lambda r: -r["total"])[: args.top]:
            mark = " [calls]" if r["total"] > r["samples"] else ""
            print(f"{r['total']:7.0f} {100 * r['total'] / whole:5.1f}%  regs {r['registers']:3.0f}  "
                  f"{r['file']}:{r['line']}{mark}  {r['source'][:80]}")


if __name__ == "__main__":
    main()
