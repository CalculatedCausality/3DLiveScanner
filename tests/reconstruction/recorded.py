#!/usr/bin/env python3
"""Guarded geometry-only capture acquisition and deterministic host replay.

No image acquisition, installation, device writes, or production source edits.
All fixture payloads/builds/results must be outside the repository.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import tarfile
import threading
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = "3dlivescanner-geometry-fixture-v1"
MAX_FRAMES = 1000
MAX_POINTS = 1_000_000
MAX_BYTES = 512 * 1024 * 1024


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024*1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def outside(path):
    path = Path(path).resolve()
    if path == ROOT or ROOT in path.parents:
        raise ValueError("Fixture payloads/results must stay outside the repository")
    return path


def state(data):
    fields = data.decode("ascii").split()
    if len(fields) != 7:
        raise ValueError("state.txt must contain exactly seven fields")
    count, width, height = map(int, fields[:3])
    cx, cy, fx, fy = map(float, fields[3:])
    if not (0 < count <= MAX_FRAMES and 0 < width <= 8192 and 0 < height <= 8192
            and all(math.isfinite(x) for x in (cx, cy, fx, fy)) and fx > 0 and fy > 0):
        raise ValueError("Invalid state count/dimensions/calibration")
    return dict(count=count, width=width, height=height, cx=cx, cy=cy, fx=fx, fy=fy)


def names(count):
    return ["state.txt"] + [f"{i:08d}{ext}" for i in range(count) for ext in (".pcl", ".mat")]


def frame_pose(data):
    tokens = data.decode("ascii").split()
    if len(tokens) != 48:
        raise ValueError("Pose must contain exactly three column-major 4x4 matrices")
    values = [struct.unpack("<f", struct.pack("<f", float(x)))[0] for x in tokens]
    if not all(math.isfinite(x) for x in values):
        raise ValueError("Nonfinite pose")
    # COLOR_CAMERA is matrix 0; SCREEN_CAMERA need not be rigid.
    m = values[:16]
    if any(abs(m[i]) > .01 for i in (3, 7, 11)) or abs(m[15]-1) > .01:
        raise ValueError("Non-affine color-camera pose")
    for a in range(3):
        for b in range(a, 3):
            dot = sum(m[a*4+k]*m[b*4+k] for k in range(3))
            if abs(dot-(1 if a == b else 0)) > .01:
                raise ValueError("Nonrigid color-camera pose")
    determinant = (m[0]*(m[5]*m[10]-m[9]*m[6]) - m[4]*(m[1]*m[10]-m[9]*m[2])
                   + m[8]*(m[1]*m[6]-m[5]*m[2]))
    if abs(determinant-1) >= .01:
        raise ValueError("Reflected/singular color-camera pose")
    return values


def point_cloud(data):
    if len(data) < 4:
        raise ValueError("Truncated point count")
    count, = struct.unpack_from("<I", data)
    if not 0 < count <= MAX_POINTS or len(data) != 4 + count*16:
        raise ValueError("Point count/length mismatch, trailing data, or unsupported size")
    for p in struct.iter_unpack("<4f", data[4:]):
        if not all(math.isfinite(v) for v in p) or p[2] <= 0 or not 0 < p[3] <= 1:
            raise ValueError("Invalid XYZC point (same positive-depth/confidence contract as replay)")
    return count


def inspect(root) -> dict[str, Any]:
    root = outside(root)
    path = root / "state.txt"
    if path.is_symlink() or path.stat().st_size > 4096:
        raise ValueError("Invalid state file")
    metadata = state(path.read_bytes())
    expected = set(names(metadata["count"]))
    actual = {p.name for p in root.iterdir()}
    if actual - expected - {"manifest.json"} or expected - actual:
        raise ValueError("Missing or unexpected fixture files")
    files, total, points = {}, 0, 0
    for name in sorted(expected):
        path = root / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("Fixture files must be regular, non-symlink files")
        size = path.stat().st_size
        maximum = 4 + 16*MAX_POINTS if name.endswith(".pcl") else 16384
        if size > maximum or total + size > MAX_BYTES:
            raise ValueError("Fixture byte budget exceeded")
        data = path.read_bytes()
        if len(data) != size:
            raise ValueError("Fixture changed while reading")
        if name.endswith(".pcl"):
            points += point_cloud(data)
        elif name.endswith(".mat"):
            frame_pose(data)
        files[name] = dict(bytes=size, sha256=sha(data))
        total += size
    fingerprint = sha("".join(f"{name}\t{entry['bytes']}\t{entry['sha256']}\n"
                             for name, entry in sorted(files.items())).encode("ascii"))
    return dict(state=metadata, files=files, fixture_sha256=fingerprint,
                raw_file_count=len(files), bytes=total, points=points)


def freeze(root, kind, extra=None):
    info = inspect(root)
    manifest = dict(info, schema=SCHEMA, kind=kind, created_unix=time.time(),
                    captured_resolution_m=None,
                    resolution_note="No explicit scan resolution in the acquired state/pcl/mat schema")
    if extra:
        manifest.update(extra)
    with (root / "manifest.json").open("x") as out:
        json.dump(manifest, out, indent=2)
        out.write("\n")
    return manifest


def validate(root):
    root = outside(root)
    manifest_path = root / "manifest.json"
    if manifest_path.is_symlink() or manifest_path.stat().st_size > 1024*1024:
        raise ValueError("Invalid fixture manifest file")
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("schema") != SCHEMA or manifest.get("kind") not in (
            "recorded-device", "synthetic-validation-only"):
        raise ValueError("Missing/unsupported fixture manifest")
    current = inspect(root)
    for field in current:
        if current[field] != manifest.get(field):
            raise ValueError("Fixture changed or manifest mismatch: " + field)
    return manifest


def acquire(args):
    root = outside(args.output)
    if root.exists():
        raise ValueError("Acquisition destination must be new")
    adb = [str(args.adb), "-s", args.serial]
    remote = args.remote.rstrip("/")
    run_as = getattr(args, "run_as", None)
    if run_as and not re.fullmatch(r"[A-Za-z0-9_.]+", run_as):
        raise ValueError("Invalid run-as package")

    def scoped(command):
        return "run-as " + shlex.quote(run_as) + " sh -c " + shlex.quote(command) if run_as else command

    def shell(command):
        result = subprocess.run(adb + ["shell", scoped(command)], check=True, capture_output=True,
                                timeout=60)
        if len(result.stdout) > 2*1024*1024:
            raise ValueError("Unexpectedly large remote metadata output")
        return result.stdout

    def read_state():
        return shell("cat " + shlex.quote(remote + "/state.txt"))

    def inventory():
        directory = shlex.quote(remote)
        command = (f"for f in {directory}/*.pcl {directory}/*.mat; do "
                   "if [ -f \"$f\" ]; then stat -c '%n|%s|%Y' \"$f\"; fi; done; "
                   f"if [ -e {directory}/state.txt.tmp ]; then printf 'UNCOMMITTED_STATE\\n'; fi")
        records = {}
        for line in shell(command).decode("utf8").splitlines():
            if line == "UNCOMMITTED_STATE":
                raise ValueError("Uncommitted state temporary exists; acquisition aborted")
            filename, size, modified = line.rsplit("|", 2)
            name = filename.rsplit("/", 1)[-1]
            if not re.fullmatch(r"[0-9]{8}\.(pcl|mat)", name):
                raise ValueError("Unexpected frame filename")
            records[name] = (int(size), int(modified))
        return records

    first = read_state()  # Missing state aborts BEFORE any file is pulled.
    metadata = state(first)
    for field in ("count", "width", "height"):
        expected = getattr(args, "expect_" + field)
        if expected is not None and metadata[field] != expected:
            raise ValueError("Capture no longer matches expected " + field)
    expected_names = names(metadata["count"])
    before = inventory()
    committed_names = set(expected_names) - {"state.txt"}
    if not committed_names <= set(before):
        raise ValueError("Incomplete committed pcl/mat file inventory")
    if sum(before[name][0] for name in committed_names) + len(first) > MAX_BYTES:
        raise ValueError("Capture exceeds the acquisition byte budget")
    for name in committed_names:
        size = before[name][0]
        cap = 4 + MAX_POINTS*16 if name.endswith(".pcl") else 16384
        if not 0 < size <= cap:
            raise ValueError("Invalid remote file size")
    time.sleep(2)
    if read_state() != first or inventory() != before:
        raise ValueError("Capture is changing; acquisition aborted")

    def remote_hashes():
        result = {}
        for offset in range(0, len(expected_names), 32):
            batch = expected_names[offset:offset+32]
            command = "sha256sum " + " ".join(shlex.quote(remote + "/" + n) for n in batch)
            for line in shell(command).decode("utf8").splitlines():
                digest, path = line.split(None, 1)
                if not re.fullmatch("[0-9a-f]{64}", digest):
                    raise ValueError("Invalid remote checksum response")
                result[path.strip().lstrip("*").rsplit("/", 1)[-1]] = digest
        if set(result) != set(expected_names):
            raise ValueError("Incomplete remote checksums")
        return result

    hashes = remote_hashes()
    root.mkdir(parents=False, exist_ok=False)
    # Explicit allowlist: never pull a directory, image, .bin preview, or library.
    if run_as:
        # One read-only stream avoids a separate wireless round trip per file.
        # Do not extract archive paths: accept only exact allowlisted regular
        # members with the previously inventoried lengths, then hash them below.
        command = "tar -c -C " + shlex.quote(remote) + " " + " ".join(map(shlex.quote, expected_names))
        process = subprocess.Popen(adb + ["exec-out", scoped(command)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        timer = threading.Timer(180, process.kill)
        timer.start()
        seen = set()
        try:
            with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
                for member in archive:
                    if not member.isfile() or member.name not in expected_names or member.name in seen:
                        raise ValueError("Unexpected private capture archive member")
                    size = len(first) if member.name == "state.txt" else before[member.name][0]
                    if member.size != size:
                        raise ValueError("Private capture size changed during streaming")
                    stream = archive.extractfile(member)
                    with (root / member.name).open("xb") as destination:
                        remaining = size
                        while remaining:
                            data = stream.read(min(remaining, 1024*1024))
                            if not data:
                                raise ValueError("Truncated private capture stream")
                            destination.write(data); remaining -= len(data)
                    seen.add(member.name)
            process.wait(timeout=15)
            if process.returncode or seen != set(expected_names):
                raise ValueError("Private capture stream failed or omitted files")
        finally:
            timer.cancel()
            if process.poll() is None:
                process.kill(); process.wait()
            process.stdout.close(); process.stderr.close()
    else:
        for name in expected_names:
            subprocess.run(adb + ["pull", remote + "/" + name, str(root / name)], check=True,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60)
    if read_state() != first or inventory() != before or remote_hashes() != hashes or read_state() != first:
        raise ValueError("Capture changed during acquisition; partial directory is NOT a frozen fixture")
    info = inspect(root)
    if (root / "state.txt").read_bytes() != first or any(info["files"][n]["sha256"] != hashes[n] for n in expected_names):
        raise ValueError("Pulled data differs from stable remote files")
    manifest = freeze(root, "recorded-device", dict(remote_path=remote, serial=args.serial,
                        run_as_package=run_as,
                        state_before_sha256=sha(first), state_after_sha256=sha(first),
                        remote_hashes_verified_twice=True, stability_interval_seconds=2,
                        ignored_uncommitted_frame_files=len(before)-len(committed_names)))
    print(json.dumps({k: manifest[k] for k in ("kind", "fixture_sha256", "raw_file_count", "points", "bytes")}, indent=2))


def synthetic(root):
    root = outside(root)
    root.mkdir(parents=False, exist_ok=False)
    (root / "state.txt").write_text("6 48 48 23.5 23.5 50 50\n")
    for frame in range(6):
        angle = .04*frame
        c, s = math.cos(angle), math.sin(angle)
        tx, ty, tz = -.1+.03*frame, .01*math.sin(frame), 0
        matrix = [c,0,-s,0, 0,1,0,0, s,0,c,0, tx,ty,tz,1]
        projection = [1.2,0,0,0, 0,1.5,0,0, 0,0,-1.02,-1, 0,0,-.2,0]
        values = matrix + matrix + projection
        (root / f"{frame:08d}.mat").write_text("".join(
            " ".join(f"{v:.6f}" for v in values[i:i+4])+"\n" for i in range(0,48,4)))
        points = []
        for y in range(48):
            for x in range(48):
                ux, uy = (x-23.5)/50, (y-23.5)/50
                ray = (c*ux+s, uy, -s*ux+c)
                depth = (2-tz)/ray[2]
                near, far = 0., 100.
                for origin, direction, low, high in zip((tx,ty,tz),ray,(.1,-.2,1.2),(.45,.2,1.6)):
                    if abs(direction) < 1e-12:
                        if not low <= origin <= high:
                            far = -1
                    else:
                        a, b = sorted(((low-origin)/direction,(high-origin)/direction))
                        near, far = max(near,a), min(far,b)
                if 0 < near < far:
                    depth = min(depth,near)
                depth += .001*math.sin(x*7+y*11+frame)
                points.append((ux*depth,uy*depth,depth,.7+.3*((x+y)%5)/4))
        with (root / f"{frame:08d}.pcl").open("wb") as out:
            out.write(struct.pack("<I",len(points)))
            for p in points:
                out.write(struct.pack("<4f",*p))
    return freeze(root,"synthetic-validation-only",dict(generator="recorded.py synthetic v1",
                  disclaimer="Generated validation data; NOT the unavailable Pixel recording"))


def production_pose():
    source = (ROOT / "common/tango/texturize.cc").read_text()
    signature = "Tango3DR_Pose TangoTexturize::Extract3DRPose(glm::mat4 matrix)"
    start = source.index("{",source.index(signature))
    depth, end = 1, start+1
    while depth:
        if source[end] == "{": depth += 1
        if source[end] == "}": depth -= 1
        end += 1
    return source[start:end]


def source_hashes(core_source=None):
    core_source = (core_source or ROOT / "reconstruction/core.cc").resolve()
    paths = ["common/data/dataset.cc", "common/data/dataset.h",
             "common/tango/texturize.cc", "common/arcore/geometry_validation.h",
             "common/data/image.h", "common/gl/opengl.h",
             "third_party/tango_3d_reconstruction/include/tango_3d_reconstruction_api.h",
             "tests/reconstruction/recorded_replay.cc", "tests/reconstruction/recorded_geometry.h",
             "tests/reconstruction/recorded.py", "tests/performance/native/include/host.h",
             "tests/performance/native/include/android/log.h",
             "tests/performance/native/include/GLES2/gl2.h",
             "tests/performance/native/include/GLES2/gl2ext.h"]
    hashes = {p: sha((ROOT / p).read_bytes()) for p in paths}
    # A core snapshot includes its private implementation headers. Hash all of
    # them, including meshing/normal/fusion helpers, rather than only the pager.
    for path in [core_source, *sorted(core_source.parent.glob("*.h"))]:
        key = str(path.relative_to(ROOT)) if ROOT in path.parents else str(path)
        hashes[key] = sha_file(path)
    glm = sorted((ROOT / "third_party/glm/glm").rglob("*.hpp")) + sorted((ROOT / "third_party/glm/glm").rglob("*.inl"))
    hashes["glm_headers_aggregate"] = sha("".join(str(p.relative_to(ROOT))+"\t"+sha(p.read_bytes())+"\n" for p in glm).encode())
    return hashes


def build(output, sanitize, core_source=None):
    core_source = (core_source or ROOT / "reconstruction/core.cc").resolve()
    body = production_pose()
    adapter = output / "pose_adapter.cc"
    adapter.write_text("#include <glm/glm.hpp>\n#include <glm/gtc/quaternion.hpp>\n"
                       "#include <tango_3d_reconstruction_api.h>\n"
                       "Tango3DR_Pose recordedPose(glm::mat4 matrix) " + body + "\n")
    flags = ["-std=c++11","-O2","-g","-DANDROID","-ffunction-sections","-fdata-sections",
             "-fno-fast-math", "-include",str(ROOT / "tests/performance/native/include/host.h")]
    if sanitize:
        flags += ["-fsanitize=address,undefined","-fno-omit-frame-pointer","-fno-sanitize-recover=all","-fno-pie","-no-pie"]
    includes = ["common","reconstruction","third_party/glm","third_party/tango_3d_reconstruction/include",
                "tests/performance/native/include", "tests/reconstruction"]
    command = shlex.split(os.environ.get("CXX","g++")) + flags + ["-I"+str(ROOT / p) for p in includes]
    binary = output / "recorded_replay"
    subprocess.run(command + [str(core_source),str(ROOT / "common/data/dataset.cc"),
                   str(adapter),str(ROOT / "tests/reconstruction/recorded_replay.cc"),
                   "-Wl,--gc-sections","-pthread","-o",str(binary)],check=True)
    return binary, flags, sha(body.encode())


def replay(args):
    fixture, output = outside(args.fixture), outside(args.output)
    if fixture == output or fixture in output.parents:
        raise ValueError("Results must not modify the fixture directory")
    manifest = validate(fixture)
    sources = source_hashes(args.core_source)
    output.mkdir(parents=False, exist_ok=False)
    binary, flags, pose_hash = build(output,args.sanitize,args.core_source)
    cache = output / "cache"
    if args.paging:
        cache.mkdir(mode=0o700)
    results = []
    env = dict(os.environ,ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    for repeat in range(args.repeats):
        mesh = output / f"ordered_mesh_{repeat}.bin"
        process = subprocess.run([str(binary),str(fixture),str(mesh),str(args.resolution),str(args.max_chunks),
                                 str(cache) if args.paging else "-",str(int(args.resident_mib*1024*1024)),
                                  str(int(args.backing_mib*1024*1024)),str(args.max_logical_chunks),str(args.analysis_max_faces),
                                   str(args.max_update_chunks),str(args.max_depth),str(args.min_vertices),str(int(not args.no_clearing))],
                                 check=True,capture_output=True,text=True,env=env,timeout=900)
        result = json.loads(process.stdout)
        result["ordered_mesh_sha256"] = sha_file(mesh)
        result["diagnostics"] = process.stderr.splitlines()
        results.append(result)
        if args.paging and list(cache.iterdir()):
            raise ValueError("Paging left named files in its private cache directory")
    if source_hashes(args.core_source) != sources or validate(fixture) != manifest:
        raise ValueError("Source or frozen input changed during replay; results invalid")
    for result in results[1:]:
        for field in ("ordered_mesh_sha256","config","mesh","residuals","statuses","points","dirty_segments"):
            if result[field] != results[0][field]:
                raise ValueError("Nondeterministic replay field: " + field)
    report = dict(schema="3dlivescanner-recorded-benchmark-v1", fixture_kind=manifest["kind"],
                  fixture_sha256=manifest["fixture_sha256"], fixture_path=str(fixture),
                  input_state=manifest["state"],
                  sources=sources, pose_conversion_body_sha256=pose_hash, flags=flags,
                  compiler=subprocess.check_output(shlex.split(os.environ.get("CXX","g++"))+["--version"],text=True).splitlines()[0],
                  platform=platform.platform(), sanitizer=args.sanitize,
                  resolution_basis="explicit experimental config; captured setting is unknown",
                  capture_images_acquired=False, legacy_comparison="not performed",
                  residual_disclaimer="One-sided sampled input-to-reconstructed-surface self-consistency, NOT ground-truth accuracy",
                  deterministic_repeats=len(results), results=results)
    (output / "results.json").write_text(json.dumps(report,indent=2,allow_nan=False)+"\n")
    print(json.dumps(dict(fixture_kind=manifest["kind"],fixture_sha256=manifest["fixture_sha256"],
                         ordered_mesh_sha256=results[0]["ordered_mesh_sha256"],
                         frames=manifest["state"]["count"],points=manifest["points"],
                         report=str(output / "results.json")),indent=2))


def self_test():
    with tempfile.TemporaryDirectory(prefix="recorded-validation-",dir="/tmp/opencode") as temp:
        root = Path(temp) / "synthetic"
        synthetic(root)
        validate(root)
        cases = []
        pcl = root / "00000000.pcl"
        original = pcl.read_bytes()
        for label, changed in [("truncated count",b"\0"), ("truncated points",original[:-1]),
                               ("trailing bytes",original+b"x"), ("oversized count",struct.pack("<I",1000001)+original[4:]),
                               ("nonfinite point",original[:4]+struct.pack("<f",float("nan"))+original[8:]),
                               ("invalid confidence",original[:16]+struct.pack("<f",1.2)+original[20:])]:
            pcl.write_bytes(changed)
            try: inspect(root)
            except (ValueError,OverflowError): cases.append(label)
            else: raise AssertionError("Accepted " + label)
            pcl.write_bytes(original)
        pose_file = root / "00000000.mat"
        original_pose = pose_file.read_bytes()
        for label, data in [("truncated pose",b"1 0 0"), ("nonfinite pose",original_pose.replace(b"1.000000",b"nan",1)),
                            ("scaled pose",original_pose.replace(b"1.000000",b"2.000000",1))]:
            pose_file.write_bytes(data)
            try: inspect(root)
            except (ValueError,OverflowError): cases.append(label)
            else: raise AssertionError("Accepted " + label)
            pose_file.write_bytes(original_pose)
        pcl.rename(root / "missing.pcl")
        try: inspect(root)
        except ValueError: cases.append("missing/misnamed frame")
        else: raise AssertionError("Accepted missing frame")
        (root / "missing.pcl").rename(pcl)
        bad = bytearray(original); bad[4] ^= 1
        pcl.write_bytes(bad)
        try: validate(root)
        except ValueError: cases.append("changed fixture hash")
        else: raise AssertionError("Accepted changed fixture")
        pcl.write_bytes(original)
        validate(root)
        state_path = root / "state.txt"
        original_state = state_path.read_bytes()
        for label, data in [("truncated state",b"6 48"), ("nonfinite intrinsics",b"6 48 48 0 0 nan 50\n"),
                            ("zero frame count",b"0 48 48 0 0 50 50\n")]:
            state_path.write_bytes(data)
            try: inspect(root)
            except ValueError: cases.append(label)
            else: raise AssertionError("Accepted " + label)
            state_path.write_bytes(original_state)
        validate(root)
        print("Recorded fixture validation passed:", ", ".join(cases))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command",required=True)
    get = commands.add_parser("acquire")
    get.add_argument("--adb",type=Path,required=True); get.add_argument("--serial",required=True)
    get.add_argument("--remote",required=True); get.add_argument("--output",type=Path,required=True)
    get.add_argument("--run-as", help="Read a debuggable app's private workspace without changing permissions")
    for field in ("count","width","height"): get.add_argument("--expect-"+field,type=int)
    gen = commands.add_parser("synthetic"); gen.add_argument("--output",type=Path,required=True)
    valid = commands.add_parser("validate"); valid.add_argument("--fixture",type=Path,required=True)
    run = commands.add_parser("run")
    run.add_argument("--fixture",type=Path,required=True); run.add_argument("--output",type=Path,required=True)
    run.add_argument("--repeats",type=int,default=2); run.add_argument("--resolution",type=float,default=.04)
    run.add_argument("--sanitize",action="store_true")
    run.add_argument("--core-source",type=Path,default=ROOT / "reconstruction/core.cc",
                     help="Frozen core.cc and sibling private headers for controlled A/B replay")
    run.add_argument("--paging",action="store_true")
    run.add_argument("--resident-mib",type=float,default=96)
    run.add_argument("--backing-mib",type=float,default=1024)
    run.add_argument("--max-logical-chunks",type=int,default=8192)
    run.add_argument("--max-chunks",type=int,default=1024,help="RAM-mode comparison cap; existing core range 1..4096")
    run.add_argument("--max-update-chunks",type=int,default=256,help="Explicit per-frame chunk budget; core default remains 256")
    run.add_argument("--max-depth",type=float,default=15,help="Explicit reconstruction depth limit in metres")
    run.add_argument("--min-vertices",type=int,default=0,help="Explicit per-segment component filter")
    run.add_argument("--no-clearing",action="store_true",help="Diagnostic replay without free-space clearing")
    run.add_argument("--analysis-max-faces",type=int,default=1000000,help="Host diagnostic budget only, not an engine limit")
    commands.add_parser("self-test")
    args = parser.parse_args()
    if args.command == "acquire": acquire(args)
    elif args.command == "synthetic":
        info = synthetic(args.output)
        print(json.dumps({k:info[k] for k in ("kind","fixture_sha256","raw_file_count","points","bytes")},indent=2))
    elif args.command == "validate":
        info = validate(args.fixture)
        print(json.dumps({k:info[k] for k in ("kind","fixture_sha256","raw_file_count","points","bytes")},indent=2))
    elif args.command == "run":
        if not math.isfinite(args.max_depth) or not 0 < args.max_depth <= 100 or not 0 <= args.min_vertices <= 1000000:
            parser.error("Invalid depth/component policy")
        if not 1 <= args.repeats <= 10 or not math.isfinite(args.resolution) or not .001 <= args.resolution <= 1:
            parser.error("Invalid repeat count/resolution")
        if not (1 <= args.max_chunks <= 4096 and 1 <= args.max_logical_chunks <= 32768 and
                1 <= args.max_update_chunks <= 1024 and
                1 <= args.analysis_max_faces <= 4000000 and math.isfinite(args.resident_mib) and
                math.isfinite(args.backing_mib) and .75 <= args.resident_mib <= 4096 and
                .09375 <= args.backing_mib <= 4096):
            parser.error("Invalid paging/comparison budgets")
        replay(args)
    else: self_test()


if __name__ == "__main__":
    try: main()
    except (OSError,ValueError,OverflowError,subprocess.SubprocessError) as error:
        print("ERROR:",error,file=sys.stderr)
        sys.exit(1)
