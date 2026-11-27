#!/usr/bin/env python3
"""Compares two benchmark results (the JSON a replay benchmark writes) and
flags what got slower.

    tools/bench_compare.py <before.json> <after.json> [--threshold 10] [--floor 0.05] [--phases a,b,c]

Every phase the two files share is compared on its p50 and its p99; the mean,
the p95 and the max are printed beside them, since a change in the middle of
a distribution says less than a change in its tail. Lower is better, so a
phase regresses when it is more than --threshold's percent slower after than
before, and improves when it is that much faster.

The renderer's passes -- what the GPU spent on the opaque drawing, the shadow
maps, the post chain and the rest -- are compared the same way, on the same
threshold and the same floor, under their own heading between the phases and
the extras.

A phase whose numbers are both under --floor milliseconds is noise: a
percentage of two roundings of zero says nothing, so those rows are printed
with their numbers and a "-" where the change would be, and never count as a
regression or an improvement. The floor is a floor on the phase, not on the
comparison: a phase that was doing nothing and now does 2 ms is a regression
however small its before was.

Outside the phases three numbers are compared and reported with the same
threshold, marked "extra" so they are never read as a phase: fpsEquivalent
(higher is better), peakRssMb and loadIn.totalMs (both lower are better) --
a frame time that stays put while the peak RSS or the load-in climbs is a
regression too. A phase or a pass only one of the two files has is listed as
such and is never a regression; neither is a run that got faster.

The two runs have to be the same build on the same machine for the numbers
to mean the same thing: a difference in commit, gpu, resolution, preset or
rayTracing is warned about, but is not an error, since the comparison is
still worth printing (a replay benchmark plays a capture, so
nothing here contacts Roblox).

Exits 0 when nothing regressed, 1 when something did, 2 on a usage or parse
error -- a missing file, a file that is not json, or one that is not a
benchmark result (no "version" and "phases" keys).
"""

import argparse
import json
import pathlib
import sys

# The percentiles a verdict is made on; the rest are printed for context.
COMPARED = ("p50", "p99")
# Milliseconds under which a phase is noise rather than a change.
DEFAULT_FLOOR = 0.05
# Every percentile of a phase, in the order a table reads best.
STATS = ("mean", "p50", "p99", "p95", "max")

# The numbers outside the phases, with the way round that counts as worse.
# Each holds the paths to the number, tried in order: the first the file has.
EXTRAS = (
    ("fpsEquivalent", (("fpsEquivalent",),), False, "fps"),
    ("peakRssMb", (("peakRssMb",),), True, "MiB"),
    ("loadIn.totalMs", (("loadIn", "totalMs"), ("loadIn", "total")), True, "ms"),
)

# What the two runs have to agree on for the comparison to be worth anything.
WARNED = ("commit", "gpu", "resolution", "preset", "rayTracing")

# The whole frame first, since it is the number anyone quotes, then the rest
# in the order the frame spends its time in.
def phases_of(data):
    """The run's phases; a result with no usable ones gives an empty mapping
    rather than a traceback part way through the table."""
    phases = data.get("phases")
    return phases if isinstance(phases, dict) else {}


def phase_names(before, after):
    """The phases the two runs have between them, the frame first and the rest
    by name."""
    names = sorted(set(phases_of(before)) | set(phases_of(after)))
    if "frame" in names:
        names.remove("frame")
        names.insert(0, "frame")
    return names


def passes_of(data):
    """The run's render passes, the same summaries its phases hold. A run whose
    GPU could not time them has no passes at all, which gives an empty mapping
    rather than a traceback part way through the table."""
    passes = data.get("passes")
    return passes if isinstance(passes, dict) else {}


def pass_names(before, after):
    """The passes the two runs have between them: in the order the renderer
    records them where a result names them, by name for whatever is left."""
    every = sorted(set(passes_of(before)) | set(passes_of(after)))
    order = before.get("passNames") or after.get("passNames")
    if not isinstance(order, list):
        return every
    ranked = [name for name in order if name in every]
    ranked += [name for name in every if name not in ranked]
    return ranked


def resolution(data):
    """The run's resolution, as the binary wrote it: one string, or a width
    and a height."""
    if "resolution" in data:
        return data["resolution"]
    if "width" in data and "height" in data:
        return f"{data['width']}x{data['height']}"
    return None


def metadata(data):
    """How the run describes itself, for the same order in both columns. The
    ray tracing flag is the setting; what the run measured of it is under
    rayTracingStats, of its own."""
    ray_tracing = data.get("rayTracing")
    return {
        "recording": data.get("recording"),
        "commit": data.get("commit"),
        "build": data.get("build"),
        "gpu": data.get("gpu"),
        "resolution": resolution(data),
        "preset": data.get("preset"),
        "rayTracing": ray_tracing if isinstance(ray_tracing, bool) else None,
    }


def scalar(data, paths):
    """The number at the first of `paths` the result holds, where a result may
    keep it directly or inside a summary of its own. None where it has no
    such number."""
    for path in paths:
        value = data
        for key in path:
            if not isinstance(value, dict) or key not in value:
                value = None
                break
            value = value[key]
        if isinstance(value, dict):
            # A summary: its mean is the number, or its middle if it has none.
            for key in ("mean", "p50"):
                if isinstance(value.get(key), (int, float)):
                    return float(value[key])
        elif isinstance(value, (int, float)) and not isinstance(value, bool):
            return float(value)
    return None


def stat_of(summary, stat):
    """One percentile out of the summary of a phase or of a pass, in
    milliseconds; None where there is no summary to read it from."""
    return scalar(summary, ((stat,),)) if isinstance(summary, dict) else None


def change(before, after):
    """How much bigger `after` is than `before`, in percent. Nothing to be
    relative to (a phase that ran no frames) counts as no change."""
    return 0.0 if before == 0 else (after - before) / before * 100.0


def verdict(before, after, threshold, lower_is_better=True, floor=DEFAULT_FLOOR):
    """("regression", "improvement", "noise" or "", the change in percent or
    None where there is none worth printing): what a move from `before` to
    `after` means, on a threshold in percent."""
    if max(before, after) < floor:
        return "noise", None
    if before == 0:
        # There is no percentage of nothing: a phase that spent no time and
        # now spends some is a regression, or an improvement where higher is
        # better (as fps is).
        return ("regression" if (after > 0) == lower_is_better else "improvement"), None
    moved = change(before, after)
    if lower_is_better:
        if moved > threshold:
            return "regression", moved
        if moved < -threshold:
            return "improvement", moved
    else:
        if moved < -threshold:
            return "regression", moved
        if moved > threshold:
            return "improvement", moved
    return "", moved


def timed_rows(names, before, after, section, threshold, floor):
    """A row per percentile of each of `names` both runs have, out of the two
    runs' summaries of them: the percentiles that decide with a verdict, the
    rest for context. The names only one run has, and the ones too small to
    judge, come back alongside the rows."""
    rows, only_before, only_after, noise = [], [], [], set()
    for name in names:
        if name not in before:
            only_after.append(name)
            continue
        if name not in after:
            only_before.append(name)
            continue
        for stat in STATS:
            first = stat_of(before.get(name), stat)
            second = stat_of(after.get(name), stat)
            if first is None or second is None:
                continue
            # Only the compared percentiles get a verdict; the rest are there
            # to be read, not judged. The floor is in milliseconds, so it is
            # the phases and passes it applies to and not the extras below.
            if stat in COMPARED:
                flag, moved = verdict(first, second, threshold, floor=floor)
            elif max(first, second) < floor or first == 0:
                # A context row, whose percentage is only there to be read:
                # there is none to print when it is noise or out of nothing.
                flag, moved = "", None
            else:
                flag, moved = "", change(first, second)
            if flag == "noise" and stat in COMPARED:
                noise.add(name)  # too small on the percentiles that judge it
            rows.append({"section": section, "label": name, "stat": stat, "unit": "ms",
                         "before": first, "after": second, "delta": moved, "flag": flag})
    return rows, only_before, only_after, noise


def compare(before, after, threshold, only=None, floor=DEFAULT_FLOOR):
    """Everything the two results say: a row per phase percentile, a row per
    pass percentile, a row per extra, the phases and passes only one file has,
    and the rows that count against us. `only` limits the phases to the ones
    named on the command line; the passes and the extras are compared either
    way."""
    every = phase_names(before, after)
    # A name that is in neither run is said so: --phases scripting would
    # otherwise compare nothing and report nothing wrong.
    unknown = [] if only is None else [name for name in only if name not in every]
    names = [name for name in every if only is None or name in only]
    phases, only_before, only_after, noise = timed_rows(names, phases_of(before), phases_of(after),
                                                        "phase", threshold, floor)
    passes, pass_before, pass_after, pass_noise = timed_rows(pass_names(before, after), passes_of(before),
                                                             passes_of(after), "pass", threshold, floor)

    extras = []
    for label, paths, lower_is_better, unit in EXTRAS:
        first = scalar(before, paths)
        second = scalar(after, paths)
        if first is None or second is None:
            continue
        flag, moved = verdict(first, second, threshold, lower_is_better)
        extras.append({"section": "extra", "label": label, "stat": "", "before": first, "after": second,
                       "delta": moved, "flag": flag, "unit": unit,
                       "direction": "higher is better" if not lower_is_better else "lower is better"})

    # The phases and passes in one file only are reported, never judged: one
    # that appeared or went away is a change in what was measured, not a
    # slowdown.
    rows = phases + passes + extras
    return {
        "phases": phases,
        "passes": passes,
        "extras": extras,
        "only_before": only_before,
        "only_after": only_after,
        "pass_only_before": pass_before,
        "pass_only_after": pass_after,
        "unknown": unknown,
        "noise": sorted(noise),
        "pass_noise": sorted(pass_noise),
        "regressions": [row for row in rows if row["flag"] == "regression"],
        "improvements": [row for row in rows if row["flag"] == "improvement"],
    }


class UsageError(Exception):
    """A missing or unreadable file, or one that is not a benchmark result."""


def load(path):
    """The result in `path`, refusing a file that is not one (an old log, a
    run's stdout, a hand-written json)."""
    try:
        text = pathlib.Path(path).read_text()
    except OSError as error:
        raise UsageError(f"{path}: {error.strerror or error}")
    try:
        data = json.loads(text)
    except json.JSONDecodeError as error:
        raise UsageError(f"{path}: not json: line {error.lineno} column {error.colno}: {error.msg}")
    if not isinstance(data, dict) or "version" not in data or "phases" not in data:
        raise UsageError(f"{path}: not a benchmark result: no 'version' and 'phases' keys")
    return data


def decimals_for(unit):
    """How many decimals a unit is worth reading at."""
    return 1 if unit == "fps" else 2


def number(value, decimals=2):
    """A number as it is printed in the tables."""
    return f"{value:.{decimals}f}"


def name_of(row):
    """A row's label, with its percentile: "frame p99", or just the label
    where the row is not a percentile of a phase."""
    return row["label"] + (f" {row['stat']}" if row["stat"] else "")


def delta_text(row):
    """A row's change in percent, or "-" where there is none to print (a row
    under the floor, or a phase that had nothing to take a percentage of)."""
    return "-" if row["delta"] is None else f"{row['delta']:+.1f}%"


def cells(row):
    """One row of a table: the label, the two numbers with their unit, the
    change in percent and what it means."""
    decimals = decimals_for(row["unit"])
    return [name_of(row), number(row["before"], decimals) + f" {row['unit']}",
            number(row["after"], decimals) + f" {row['unit']}", delta_text(row), row["flag"] or "-"]


def table(rows, headers):
    """The rows as a table, every column padded to its widest cell and the
    last one (the verdict) left as it is."""
    widths = [max(len(row[column]) for row in [headers, *rows]) for column in range(len(headers) - 1)]
    lines = []
    for row in [headers, *rows]:
        padded = "  ".join(cell.ljust(widths[index]) for index, cell in enumerate(row[:-1]))
        lines.append(f"  {padded}  {row[-1]}".rstrip())
    return lines


def show(text, out=sys.stdout):
    """Print a line."""
    print(text, file=out)


def run_line(data, path, label):
    """One run in a line of its own: how many frames it measured, what a frame
    cost it and what that is an fps. A run that reported no frame phase says
    so instead of printing a zero."""
    frame = phases_of(data).get("frame", {})
    frame = frame if isinstance(frame, dict) else {}
    mean = frame.get("mean")
    mean = number(mean) if isinstance(mean, (int, float)) else "-"
    fps = scalar(data, (("fpsEquivalent",),))
    rate = f" ({number(fps, 1)} fps)" if fps is not None else ""
    return f"{label:<7} {path}  {frame.get('frames', '?')} frames, frame mean {mean} ms{rate}"


def report(before, after, result, args, out):
    """What the comparison found, as a person reads it: what the runs were,
    the phases, the extras, and then the verdict."""
    show(run_line(before, args.before, "before"), out)
    show(run_line(after, args.after, "after"), out)
    show("", out)

    if args.phases:
        show(f"phases: {', '.join(args.phases)} only, at the given threshold of {args.threshold:g}%"
             " (the passes and the extras are compared either way)", out)
    else:
        show(f"phases (ms, lower is better; {' and '.join(COMPARED)} decide, the rest are context)", out)
    if result["noise"]:
        show(f"   under {args.floor:g} ms: {', '.join(result['noise'])} is noise, printed without a percentage", out)
    if result["unknown"]:
        show(f"   in neither run, so not compared: {', '.join(result['unknown'])}", out)
    if result["phases"]:
        show("", out)
        for line in table([cells(row) for row in result["phases"]],
                          ["phase", "before", "after", "delta", ""]):
            show(line, out)
    else:
        show("   none the two runs share", out)

    # The renderer's passes, on the same rules as the phases: the GPU's own
    # time, split the way the renderer splits it.
    show("", out)
    show(f"passes (ms, lower is better; {' and '.join(COMPARED)} decide, the rest are context)", out)
    if result["pass_noise"]:
        show(f"   under {args.floor:g} ms: {', '.join(result['pass_noise'])} is noise,"
             " printed without a percentage", out)
    if result["passes"]:
        show("", out)
        for line in table([cells(row) for row in result["passes"]],
                          ["pass", "before", "after", "delta", ""]):
            show(line, out)
    else:
        show("   none the two runs share", out)

    show("", out)
    show("extras (not phases, but counted on the same threshold)", out)
    if result["extras"]:
        show("", out)
        for line in table([cells(row) for row in result["extras"]],
                          ["metric", "before", "after", "delta", ""]):
            show(line, out)
    else:
        show("   none the two runs have", out)

    # A phase or a pass in only one of the files is a change in what was
    # measured.
    if (result["only_before"] or result["only_after"]
            or result["pass_only_before"] or result["pass_only_after"]):
        show("", out)
        show("in only one of the runs (reported, never a regression)", out)
        if result["only_before"]:
            show(f"   before only: {', '.join(result['only_before'])}", out)
        if result["only_after"]:
            show(f"   after only:  {', '.join(result['only_after'])}", out)
        if result["pass_only_before"]:
            show(f"   pass before only: {', '.join(result['pass_only_before'])}", out)
        if result["pass_only_after"]:
            show(f"   pass after only:  {', '.join(result['pass_only_after'])}", out)

    show_metadata(before, after, out)

    show("", out)
    for title, key in (("regressions", "regressions"), ("improvements", "improvements")):
        rows = result[key]
        if not rows:
            continue
        show(f"{title} ({len(rows)})", out)
        for row in rows:
            decimals = decimals_for(row["unit"])
            detail = f"{number(row['before'], decimals)} -> {number(row['after'], decimals)} {row['unit']}"
            # An extra says which way round it counts, so a run that lost its
            # memory is not read as a phase that got slower.
            kind = f" [extra, {row['direction']}]" if row["section"] == "extra" else ""
            show(f"   {name_of(row)} {delta_text(row)} ({detail}){kind}", out)
        show("", out)

    if result["regressions"]:
        show(f"{len(result['regressions'])} regression(s)", out)
    else:
        show("no regressions", out)


def shorten(text, width=44):
    """A value as the table can hold it: the tail of a long path, which is
    the part that tells two recordings apart."""
    return text if len(text) <= width else "..." + text[-(width - 3):]


def show_metadata(before, after, out):
    """How the two runs described themselves, and a warning for the things
    that make the numbers mean different work."""
    info, other = metadata(before), metadata(after)
    rows = []
    for key, value in info.items():
        first = value if value is not None else "-"
        second = other.get(key)
        second = second if second is not None else "-"
        if isinstance(first, bool):
            first, second = ("yes" if first else "no"), ("yes" if second else "no")
        elif isinstance(second, bool):
            second = "yes" if second else "no"
        first, second = shorten(str(first)), shorten(str(second))
        differs = key in WARNED and first != second
        rows.append([key, first, second, "differs" if differs else ""])

    show("", out)
    show("what the runs were", out)
    for line in table(rows, ["field", "before", "after", ""]):
        show(line, out)

    # A warning, not an error: the numbers still say something, they just say
    # it about two different things, and the exit code is the caller's to make.
    differing = [row[0] for row in rows if row[3]]
    if differing:
        show("", out)
        show(f"warning: {', '.join(differing)} differ, so this is not the same build on the same "
             f"settings:", out)
        show("   a difference here is not necessarily a regression", out)


def parse_args(argv):
    """The command line: two results, how much of a move counts, and which
    phases to look at."""
    parser = argparse.ArgumentParser(
        prog="tools/bench_compare.py",
        description="Compares two benchmark results and flags regressions.",
        epilog="Exits 0 with no regression, 1 with one, 2 on a usage or parse error.")
    parser.add_argument("before", metavar="<before.json>", help="the result to compare against")
    parser.add_argument("after", metavar="<after.json>", help="the result to compare")
    parser.add_argument("--threshold", type=float, default=10.0, metavar="PERCENT",
                        help="how much worse a number has to be to be a regression (default 10)")
    parser.add_argument("--floor", type=float, default=DEFAULT_FLOOR, metavar="MS",
                        help="phases under this many milliseconds are noise: no verdict, no percentage"
                             f" (default {DEFAULT_FLOOR})")
    parser.add_argument("--phases", metavar="a,b,c",
                        help="only these phases (the passes and the extras are compared either way)")
    args = parser.parse_args(argv)
    if args.threshold < 0:
        parser.error(f"--threshold {args.threshold}: a percentage, 0 or more")
    if args.floor < 0:
        parser.error(f"--floor {args.floor}: milliseconds, 0 or more")
    if args.phases:
        args.phases = [name.strip() for name in args.phases.split(",") if name.strip()]
    else:
        args.phases = None
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        before = load(args.before)
        after = load(args.after)
    except UsageError as error:
        print(f"bench_compare.py: {error}", file=sys.stderr)
        return 2

    result = compare(before, after, args.threshold, args.phases, args.floor)
    report(before, after, result, args, sys.stdout)
    return 1 if result["regressions"] else 0


if __name__ == "__main__":
    sys.exit(main())