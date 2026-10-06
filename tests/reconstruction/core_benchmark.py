#!/usr/bin/env python3
"""Freeze the exact current core, then compare/time it against a candidate.

Capture BEFORE editing core.cc:
  python3 tests/reconstruction/core_benchmark.py --capture /tmp/opencode/core-before
Run baseline/candidate with the same compiler, fixture and flags:
  python3 tests/reconstruction/core_benchmark.py --baseline /tmp/opencode/core-before \
      --output /tmp/opencode/core-comparison
All generated files stay in the explicit output directory, never the workspace.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "reconstruction/core.cc"
CXX = shlex.split(os.environ.get("CXX", "g++"))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def machine():
    info = {"platform": platform.platform(), "compiler": subprocess.check_output(
        CXX + ["--version"], text=True).splitlines()[0]}
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                info["cpu"] = line.split(":",1)[1].strip()
                break
    return info


def capture(destination):
    destination.mkdir(parents=False, exist_ok=False)
    records = {}
    capture_files = ["reconstruction/core.cc", "tests/reconstruction/core_test.cc",
                     "tests/reconstruction/core_run.py",
                     "third_party/tango_3d_reconstruction/include/tango_3d_reconstruction_api.h"]
    capture_files += [name for name in ("reconstruction/paging.h", "reconstruction/paging_store.h")
                      if (ROOT / name).exists()]
    for name in capture_files:
        data = (ROOT / name).read_bytes()
        with (destination / Path(name).name).open("xb") as out:
            out.write(data)
        records[name] = digest(data)
    manifest = dict(machine(), captured=time.time(), files=records)
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    assert (destination / "core.cc").read_bytes() == SOURCE.read_bytes()
    print(json.dumps(manifest, indent=2), flush=True)


def build(source, binary, sanitize):
    flags = ["-std=c++11", "-O2", "-g", "-Wall", "-Wextra", "-Werror"]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                  "-fno-sanitize-recover=all", "-fno-pie", "-no-pie"]
    subprocess.run(CXX + flags + [
        "-I" + str(ROOT / "third_party/tango_3d_reconstruction/include"),
        "-I" + str(ROOT / "third_party/glm"), str(source),
        str(ROOT / "tests/reconstruction/core_benchmark.cc"), "-o", str(binary)], check=True)
    return flags


def equivalent(a, b):
    total = 0
    with a.open("rb") as left, b.open("rb") as right:
        while True:
            x, y = left.read(1024 * 1024), right.read(1024 * 1024)
            if x != y:
                raise AssertionError("byte mismatch at/after offset %d: %s vs %s" % (total, a, b))
            if not x:
                return total
            total += len(x)


def summarize(runs):
    out = {}
    for metric in ["update_ms", "extract_ms"]:
        samples = [v for run in runs for v in run[metric]]
        ordered = sorted(samples)
        out[metric] = {"median": statistics.median(samples),
                       "p95": ordered[min(len(ordered)-1, int(.95*len(ordered)))],
                       "total_median": statistics.median(sum(run[metric]) for run in runs)}
    out["combined_total_median_ms"] = statistics.median(
        sum(run["update_ms"]) + sum(run["extract_ms"]) for run in runs)
    out["statuses"] = runs[0]["statuses"]
    out["segments"] = runs[0]["segments"]
    out["faces"] = runs[0]["faces"]
    return out


def check_repeated_output(runs):
    for run in runs[1:]:
        for key in ("statuses", "segments", "faces", "bytes", "points_per_frame"):
            assert run[key] == runs[0][key], (key, run[key], runs[0][key])


def verify_output(directory, candidate=None):
    report = json.loads((directory / "results.json").read_text())
    if candidate is not None:
        assert digest(candidate.read_bytes()) == report["candidate_sha256"], "Candidate source changed since the measured run"
    total = 0
    for case, variants in report["runs"].items():
        for runs in variants.values():
            check_repeated_output(runs)
        if "before" in variants and "after" in variants:
            total += equivalent(directory / ("before-case%s.bin" % case),
                                directory / ("after-case%s.bin" % case))
    print("Verified %d compared bytes and repeat-invariant statuses/counts." % total)


def compare(args):
    args.output.mkdir(parents=False, exist_ok=False)
    manifest = json.loads((args.baseline / "manifest.json").read_text())
    baseline = args.baseline / "core.cc"
    assert digest(baseline.read_bytes()) == manifest["files"]["reconstruction/core.cc"]
    assert digest((ROOT / "third_party/tango_3d_reconstruction/include/tango_3d_reconstruction_api.h").read_bytes()) == manifest["files"]["third_party/tango_3d_reconstruction/include/tango_3d_reconstruction_api.h"]
    variants = {"before": baseline}
    if not args.baseline_only:
        variants["after"] = args.candidate or SOURCE
    report: dict[str, Any] = dict(machine(), baseline_manifest=manifest, flags=None,
                                  pinned_cpu=args.cpu, runs={}, summary={})
    binaries = {}
    for name, source in variants.items():
        binaries[name] = args.output / name
        report["flags"] = build(source, binaries[name], args.sanitize)
    report["candidate_sha256"] = digest((args.candidate or SOURCE).read_bytes())
    report["fixture_sha256"] = digest((ROOT / "tests/reconstruction/core_benchmark.cc").read_bytes())
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    pin = None
    if args.cpu is not None:
        if args.cpu not in os.sched_getaffinity(0):
            raise ValueError("Requested CPU is not in this process's allowed affinity")
        pin = lambda: os.sched_setaffinity(0, {args.cpu})
    # Alternate order to reduce warm-up / frequency / concurrent-load bias.
    for repeat in range(args.repeats):
        order = list(variants)
        if repeat % 2:
            order.reverse()
        for case in args.cases:
            for name in order:
                dump = args.output / ("%s-case%d.bin" % (name, case)) if repeat == 0 else None
                run = subprocess.run([str(binaries[name]), str(dump) if dump else "-",
                                      str(args.frames), str(case)], check=True,
                                     capture_output=True, text=True, env=env, preexec_fn=pin)
                result = json.loads(run.stdout)
                report["runs"].setdefault(str(case), {}).setdefault(name, []).append(result)
                if run.stderr:
                    print(name, "case", case, run.stderr, flush=True)
            if repeat == 0 and "after" in variants:
                size = equivalent(args.output / ("before-case%d.bin" % case),
                                  args.output / ("after-case%d.bin" % case))
                print("case %d: %d serialized bytes identical" % (case, size), flush=True)
    for case, runs in report["runs"].items():
        for samples in runs.values():
            check_repeated_output(samples)
        report["summary"][case] = {name: summarize(samples) for name, samples in runs.items()}
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2), flush=True)


parser = argparse.ArgumentParser()
parser.add_argument("--capture", type=Path)
parser.add_argument("--verify-output", type=Path)
parser.add_argument("--baseline", type=Path)
parser.add_argument("--output", type=Path)
parser.add_argument("--candidate", type=Path)
parser.add_argument("--cpu", type=int, help="Pin measured child processes to one allowed Linux CPU")
parser.add_argument("--baseline-only", action="store_true")
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--frames", type=int, default=24)
parser.add_argument("--repeats", type=int, default=3)
parser.add_argument("--cases", type=int, nargs="+", default=[0, 1, 2])
args = parser.parse_args()
if args.capture:
    capture(args.capture)
elif args.verify_output:
    verify_output(args.verify_output, args.candidate)
else:
    if not args.baseline or not args.output or args.frames < 1 or args.repeats < 1:
        parser.error("--baseline, --output and positive --frames/--repeats are required")
    compare(args)
