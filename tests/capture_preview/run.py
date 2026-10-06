#!/usr/bin/env python3
"""Bounded synthetic native preview ABBA and fine-replay isolation checks.

Reports/binaries/input/mesh dumps must be in /tmp/opencode. Android mode uses
only a unique shell-UID scratch directory and removes it even on failure.
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
import uuid

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def summarize(runs):
    result = {}
    for metric in ("update_ms", "extract_ms"):
        values = [v for r in runs for v in r[metric]]
        ordered = sorted(values)
        result[metric] = {"frame_median": statistics.median(values),
                          "frame_p95": ordered[min(len(ordered)-1, int(.95*len(ordered)))],
                          "total_median": statistics.median(sum(r[metric]) for r in runs)}
    result["combined_total_median_ms"] = statistics.median(
        sum(r["update_ms"]) + sum(r["extract_ms"]) for r in runs)
    for key in ("peak_process_rss_kib", "resident_chunk_bytes", "peak_resident_chunk_bytes",
                "final_mesh_payload_bytes", "final_vertices", "final_faces", "logical_chunks"):
        result[key] = statistics.median(r[key] for r in runs)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, choices=range(1, 13), default=12)
    parser.add_argument("--fine", type=float, choices=(.01, .02), default=.01)
    parser.add_argument("--cpu", type=int, help="Host CPU affinity only")
    parser.add_argument("--sanitize", action="store_true", help="Host correctness run, timings not performance")
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--serial")
    args = parser.parse_args()
    if bool(args.sdk) != bool(args.serial):
        parser.error("Android requires both --sdk and --serial")
    if args.serial and (args.cpu is not None or args.sanitize):
        parser.error("Android runs use default scheduling and no sanitizer")
    output = args.output.resolve()
    if Path("/tmp/opencode") not in output.parents:
        parser.error("--output must be a new directory under /tmp/opencode")
    output.mkdir(exist_ok=False)
    if args.cpu is not None and args.cpu not in os.sched_getaffinity(0):
        parser.error("CPU is not allowed by host affinity")
    sources = [ROOT / "reconstruction/core.cc", *sorted((ROOT / "reconstruction").glob("*.h")),
               ROOT / "tests/reconstruction/core_benchmark.cc", Path(__file__).resolve(),
               ROOT / "tests/capture_preview/benchmark.cc",
               ROOT / "third_party/tango_3d_reconstruction/include/tango_3d_reconstruction_api.h"]
    sources += sorted(p for p in (ROOT / "third_party/glm/glm").rglob("*") if p.is_file())
    source_hashes = {str(p.relative_to(ROOT)): sha(p) for p in sources}
    compiler = ([(str(args.sdk / "ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++"))]
                if args.serial else shlex.split(os.environ.get("CXX", "g++")))
    command = compiler + ["-std=c++11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-fast-math",
        "-I" + str(ROOT / "third_party/glm"), "-I" + str(ROOT / "third_party/tango_3d_reconstruction/include"),
        str(ROOT / "reconstruction/core.cc"), str(ROOT / "tests/capture_preview/benchmark.cc")]
    if args.serial:
        command += ["-static-libstdc++", "-llog", "-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all", "-fno-pie", "-no-pie"]
    command += ["-o", str(output / "benchmark")]
    report = {"scope": "Synthetic production-core preview workload, not whole-app FPS or output quality",
              "platform": platform.platform(), "source_sha256": source_hashes,
              "compiler": subprocess.check_output(compiler + ["--version"], text=True),
              "compile_command": command, "frames": args.frames, "fine_resolution_m": args.fine,
              "coarse_resolution_m": .05, "cpu": args.cpu, "sanitized": args.sanitize,
              "order": ["fine", "coverage", "coverage", "fine"], "runs": {},
              "paging": "RAM context, default production capacity/work guards",
              "timing": "steady_clock around update and each dirty-segment extraction; validation, serialization, destruction and final snapshot excluded",
              "memory": "core resident/peak chunk payload; aggregate final mesh payload; process peak RSS includes fixture/validation/allocator overhead",
              "started_unix": time.time()}
    write_json(output / "manifest.json", report)
    subprocess.run(command, check=True, timeout=180)
    adb = [str(args.sdk / "platform-tools/adb"), "-s", args.serial] if args.serial else None
    remote = "/data/local/tmp/capture-preview-" + uuid.uuid4().hex
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def adb_run(*cmd):
        assert adb is not None
        return subprocess.check_output(adb + list(cmd), text=True, timeout=30).strip()

    def invoke(label, resolution, mode):
        prefix = remote if adb else str(output)
        mesh = prefix + "/" + label + ".mesh"
        inputs = prefix + "/" + label + ".input"
        cmd = [prefix + "/benchmark", str(resolution), str(args.frames), mode, mesh, inputs]
        if adb:
            # Lower scheduling priority; leave running apps, CPUs and governors alone.
            cmd = adb + ["shell", "nice", "-n", "10"] + cmd
        elif args.cpu is not None:
            cmd = ["taskset", "-c", str(args.cpu)] + cmd
        proc = subprocess.run(cmd, check=True, text=True, capture_output=True, env=env, timeout=120)
        row = json.loads(proc.stdout)
        row["command"] = cmd
        (output / (label + ".stderr")).write_text(proc.stderr)
        if adb:
            for ext in ("mesh", "input"):
                adb_run("pull", prefix + "/" + label + "." + ext, str(output / (label + "." + ext)))
        row["mesh_sha256"] = sha(output / (label + ".mesh"))
        row["input_sha256"] = sha(output / (label + ".input"))
        assert row["statuses"] == [0] * args.frames
        assert row["input_unchanged"] and row["finite_valid_geometry"]
        assert row["input_fnv1a_before"] == row["input_fnv1a_after"]
        assert row["input_bytes"] == (output / (label + ".input")).stat().st_size
        write_json(output / (label + ".json"), row)
        report["runs"][label] = row
        print(label, "update=%.2fms extract=%.2fms" % (sum(row["update_ms"]), sum(row["extract_ms"])), flush=True)
        return row

    created_remote = False
    try:
        if adb:
            report["device"] = {"serial": args.serial,
                                "model": adb_run("shell", "getprop", "ro.product.model"),
                                "abi": adb_run("shell", "getprop", "ro.product.cpu.abi"),
                                "kernel": adb_run("shell", "uname", "-a")}
            adb_run("shell", "ls", "-d", "/data/local/tmp")
            adb_run("shell", "mkdir", remote); created_remote = True
            adb_run("push", str(output / "benchmark"), remote + "/benchmark")
            adb_run("shell", "chmod", "700", remote + "/benchmark")
        for i, name in enumerate(report["order"]):
            invoke(f"{i}-{name}", args.fine if name == "fine" else .05, "preview")
            if adb:
                time.sleep(1)
        direct = invoke("replay-direct", args.fine, "replay")
        after = invoke("replay-after-coarse", args.fine, "after-coarse")
        assert (output / "replay-direct.mesh").read_bytes() == (output / "replay-after-coarse.mesh").read_bytes()
        assert direct["mesh_sha256"] == after["mesh_sha256"]
        assert len({r["input_sha256"] for r in report["runs"].values()}) == 1
        for label in report["runs"]:
            assert (output / (label + ".input")).read_bytes() == (output / "replay-direct.input").read_bytes()
        fine_runs = [report["runs"][f"{i}-fine"] for i in (0, 3)]
        coarse_runs = [report["runs"][f"{i}-coverage"] for i in (1, 2)]
        assert len({r["mesh_sha256"] for r in fine_runs + [direct, after]}) == 1
        assert coarse_runs[0]["mesh_sha256"] == coarse_runs[1]["mesh_sha256"]
        report["guards"] = {"all_frames_accepted": True, "inputs_byte_identical": True,
                            "repeated_preview_mesh_byte_identical": True,
                            "fine_replay_before_after_coarse_byte_identical": True,
                            "fine_replay_matches_fine_live_final_mesh": True,
                            "fine_replay_mesh_sha256": direct["mesh_sha256"],
                            "fine_replay_compared_bytes": (output / "replay-direct.mesh").stat().st_size}
        report["summary"] = {"fine": summarize(fine_runs), "coverage": summarize(coarse_runs)}
        a, b = report["summary"]["fine"], report["summary"]["coverage"]
        ratios = {}
        for key in ("update_ms", "extract_ms"):
            x, y = a[key]["total_median"], b[key]["total_median"]
            ratios[key] = {"speedup": x/y, "reduction_percent": 100*(1-y/x)}
        for key in ("combined_total_median_ms", "peak_process_rss_kib", "resident_chunk_bytes",
                    "peak_resident_chunk_bytes", "final_mesh_payload_bytes", "final_vertices", "final_faces"):
            x, y = a[key], b[key]
            ratios[key] = {"ratio_fine_over_coverage": x/y, "reduction_percent": 100*(1-y/x)}
        report["comparison"] = ratios
    finally:
        if created_remote:
            adb_run("shell", "rm", "-rf", remote)
        report["sources_unchanged"] = all(sha(ROOT / name) == digest for name, digest in source_hashes.items())
        report["finished_unix"] = time.time()
        write_json(output / "results.json", report)
    assert report["sources_unchanged"], "Sources changed during benchmark"
    print(json.dumps({"guards": report["guards"], "summary": report["summary"], "comparison": report["comparison"]}, indent=2))
    print("PASS:", output / "results.json", flush=True)


if __name__ == "__main__":
    main()
