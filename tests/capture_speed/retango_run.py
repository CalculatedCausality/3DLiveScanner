#!/usr/bin/env python3
"""Same-working-tree-baseline Retango differential, sanitizers and host timings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True,
                        help="Directory containing pre-edit retango.cc and retango.h; never git HEAD")
    parser.add_argument("--output", type=Path,
                        help="Results/build directory (default: unique /tmp/opencode/retango-results-*)")
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--benchmark-scale", type=int, default=10,
                        help="Timed-loop multiplier (1..1000), default 10")
    parser.add_argument("--no-sanitizers", action="store_true")
    parser.add_argument("--checks-only", action="store_true", help="Skip timing samples")
    parser.add_argument("--ndk", type=Path, help="Optional NDK root for arm64 Android compile check")
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    if not 1 <= args.benchmark_scale <= 1000:
        parser.error("--benchmark-scale must be between 1 and 1000")
    baseline = args.baseline.resolve()
    for name in ("retango.cc", "retango.h"):
        if not (baseline / name).is_file():
            parser.error(f"Missing captured baseline: {baseline / name}")
    work = args.output.resolve() if args.output else Path(tempfile.mkdtemp(
        prefix="retango-results-", dir="/tmp/opencode"))
    work.mkdir(parents=True, exist_ok=True)
    log = (work / "commands.log").open("w")

    def run(command):
        command = list(map(str, command))
        text = "+ " + shlex.join(command)
        print(text, flush=True)
        log.write(text + "\n")
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=300,
                                env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
        log.write(result.stdout)
        log.flush()
        if result.returncode:
            print(result.stdout, flush=True)
            result.check_returncode()
        return result.stdout

    # Freeze both sides and their hashes for reruns, including the baseline header
    # selected by its own tango/ include directory. No algorithm source rewriting.
    roots = {}
    hashes = {}
    for label, source in (("before", baseline), ("after", ROOT / "common/tango")):
        directory = work / label / "tango"
        directory.mkdir(parents=True, exist_ok=True)
        roots[label] = directory.parent
        hashes[label] = {}
        for name in ("retango.cc", "retango.h"):
            dest = directory / name
            dest.write_bytes((source / name).read_bytes())
            hashes[label][name] = sha(dest)

    cxx = shlex.split(os.environ.get("CXX", "c++"))
    common = ["-std=c++11", "-O3", "-Wall", "-Wextra", "-Wno-deprecated-declarations"]
    includes = []
    for directory in (ROOT / "tests/geometry/include", ROOT / "common",
                      ROOT / "third_party/glm", ROOT / "third_party/delaunay",
                      ROOT / "third_party/tango_3d_reconstruction/include"):
        includes += ["-I", directory]
    binaries = {}
    modes = ["optimized"] + ([] if args.no_sanitizers else ["sanitized"])
    differential = {}
    for mode in modes:
        states = {}
        for label in ("before", "after"):
            executable = work / f"{label}-{mode}"
            flags = common if mode == "optimized" else [
                "-std=c++11", "-O1", "-g", "-Wno-deprecated-declarations",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
            run(cxx + flags + ["-I", roots[label]] + includes
                + (["-DRETANGO_LAZY"] if label == "after" else [])
                + [HERE / "retango_test.cc", roots[label] / "tango/retango.cc", "-o", executable])
            state = work / f"{label}-{mode}.bin"
            print(run([executable, state]), end="", flush=True)
            states[label] = state
            if mode == "optimized":
                binaries[label] = executable
        if states["before"].read_bytes() != states["after"].read_bytes():
            raise SystemExit(f"FAIL: {mode} before/after state differs; inspect {work}")
        differential[mode] = {"sha256": sha(states["after"]), "bytes": states["after"].stat().st_size}
        print(f"PASS: {mode} byte-identical output, estimates and mask states", flush=True)

    android = None
    if args.ndk:
        compiler = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++"
        android = run([compiler, "-std=c++11", "-O3", "-DANDROID", "-c",
                       "-I", roots["after"], "-I", ROOT / "common",
                       "-I", ROOT / "third_party/glm", "-I", ROOT / "third_party/delaunay",
                       "-I", ROOT / "third_party/tango_3d_reconstruction/include",
                       roots["after"] / "tango/retango.cc", "-o", work / "retango-arm64.o"])
        print("PASS: Android arm64 production translation unit compiled", flush=True)

    samples = {"before": {}, "after": {}}
    cpu_samples = {"before": {}, "after": {}}
    for round_number in range(0 if args.checks_only else args.rounds):
        # Alternate order to reduce warm-up/order bias; both get identical flags.
        for label in (("before", "after") if round_number % 2 == 0 else ("after", "before")):
            text = run([binaries[label], "--benchmark", args.benchmark_scale])
            for line in text.splitlines():
                name, count, iterations, us, cpu_us = line.split(",")
                samples[label].setdefault(name, []).append(float(us))
                cpu_samples[label].setdefault(name, []).append(float(cpu_us))
    summary = {}
    for name in samples["before"]:
        before = statistics.median(samples["before"][name])
        after = statistics.median(samples["after"][name])
        summary[name] = {"before_median_us": before, "after_median_us": after,
                         "reduction_percent": 100 * (before - after) / before}
        cpu_before = statistics.median(cpu_samples["before"][name])
        cpu_after = statistics.median(cpu_samples["after"][name])
        summary[name].update({"before_cpu_median_us": cpu_before,
                              "after_cpu_median_us": cpu_after,
                              "cpu_reduction_percent": 100 * (cpu_before - cpu_after) / cpu_before})
        print(f"{name}: {before:.3f} -> {after:.3f} us/call, "
              f"{summary[name]['reduction_percent']:.2f}% reduction (host only)", flush=True)
        print(f"  process CPU: {cpu_before:.3f} -> {cpu_after:.3f} us/call, "
              f"{summary[name]['cpu_reduction_percent']:.2f}% reduction", flush=True)
    report = {"host": platform.platform(), "compiler": run(cxx + ["--version"]),
              "optimized_flags": common, "baseline_directory": str(baseline),
              "source_sha256": hashes, "differential": differential,
              "android_arm64_compiled": android is not None,
              "benchmark_rounds": 0 if args.checks_only else args.rounds,
              "benchmark_scale": args.benchmark_scale,
              "host_samples_us": samples, "host_cpu_samples_us": cpu_samples, "summary": summary}
    (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: results, exact sources, binaries and commands: {work}", flush=True)


if __name__ == "__main__":
    main()
