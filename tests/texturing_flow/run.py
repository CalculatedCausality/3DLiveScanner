#!/usr/bin/env python3
"""Real production definitions + explicitly injected C API/render/Poisson boundaries.

Builds the repository's actual libjpeg-turbo for host pixel tests. No Gradle/device.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
JAVA_BIN = Path(os.environ.get("JAVA_HOME", "/tmp/opencode/scanner-jdk17")) / "bin"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--dataset", type=Path, help="Read-only check of the generated three-frame synthetic legacy fixture")
args = parser.parse_args()


def fingerprint(path):
    return {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in path.rglob("*") if p.is_file()}


original = {}
if args.dataset:
    args.dataset = args.dataset.resolve()
    metadata = json.loads((args.dataset / "fixture.json").read_text())
    assert metadata["synthetic"] and metadata["preview_is_analytic_format_fixture"]
    assert metadata["frames"] == 3 and metadata["camera_x_metres"] == [-.08, 0, .08]
    original = fingerprint(args.dataset)


def definition(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError(signature)


def run(command, **kwargs):
    subprocess.run(command, cwd=ROOT, check=True, timeout=180, **kwargs)


with tempfile.TemporaryDirectory(prefix="texturing-flow-", dir="/tmp/opencode") as directory:
    work = Path(directory)
    jpeg = ROOT / "third_party/libjpeg-turbo"
    makefile = (jpeg / "Android.mk").read_text()
    sources = [jpeg / s for s in re.findall(r"\bsrc/[\w-]+\.c\b", makefile)]
    flags = shlex.split(" ".join(line.strip().rstrip("\\") for line in makefile.splitlines()[94:121]))
    run(["cc", "-shared", "-fPIC", "-O1", "-DSIZEOF_SIZE_T=8", *flags,
         "-I" + str(jpeg / "include"), "-I" + str(jpeg / "src"),
         "-I" + str(jpeg / "src/simd"), *map(str, sources), "-lm", "-o", str(work / "libjpeg-test.so")])

    wrapper = (ROOT / "common/tango/texturize.cc").read_text()
    wrapper = wrapper[wrapper.index("namespace oc {"):]
    header = (ROOT / "common/tango/texturize.h").read_text()
    header = header[header.index("namespace oc {"):header.rindex("#endif")]
    dataset = "\n".join(definition("common/data/dataset.cc", signature) for signature in [
        "Dataset::Dataset(", "std::string Dataset::GetFileName(", "std::vector<float> Dataset::ReadDistortion(",
        "bool Dataset::ReadPose(", "void Dataset::ReadState(", "float Dataset::ReadYaw(",
        "bool Dataset::WriteState(", "bool Dataset::WritePose(", "bool Dataset::ResetState("])
    template = (HERE / "flow_test.cc.in").read_text()
    source = template.replace("// PRODUCTION_HEADER", header).replace("// PRODUCTION_WRAPPER", wrapper)
    source = source.replace("// PRODUCTION_DEFINITIONS", "namespace oc {\n" + dataset + "\n" + "\n".join([
        definition("common/data/image.cc", "bool Image::JPG2YUV("),
        definition("common/thread/reconstr.cc", "bool Reconstruction::InitTexturing("),
        definition("scanner/app/src/main/jni/app.cc", "bool App::Texturize(")]) + "\n}")
    (work / "test.cc").write_text(source)
    for modern in (0, 1):
        run(shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++11", "-O1", "-g", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-DSCANNER_MODERN=" + str(modern),
             "-I" + str(ROOT / "common"), "-I" + str(ROOT / "third_party/glm"),
             "-I" + str(ROOT / "reconstruction"),
            "-I" + str(ROOT / "third_party/tango_3d_reconstruction/include"), "-I" + str(jpeg / "src"),
            str(work / "test.cc"), str(work / "libjpeg-test.so"), "-o", str(work / "test")])
        run([str(work / "test"), str(work)] + ([str(args.dataset)] if args.dataset else []),
            env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1", ASAN_OPTIONS="detect_leaks=1"))
        if args.dataset:
            assert fingerprint(args.dataset) == original, "Synthetic dataset was modified"
            print("PASS: supplied dataset files remain SHA-256 byte-identical", flush=True)
        print("PASS configuration branch: SCANNER_MODERN=" + str(modern), flush=True)

    # Execute the exact postprocess Service lambda with labeled Java UI/JNI/service fakes.
    main = (ROOT / "scanner/app/src/main/java/com/lvonasek/arcore3dscanner/main/Main.java").read_text()
    begin = main.index("                mGLView.stop();", main.index("Service.process(getString(R.string.postprocessing)"))
    end = main.index("\n              });", begin)
    java = (HERE / "Main.java.in").read_text().replace("// PRODUCTION_POSTPROCESS", main[begin:end])
    (work / "Main.java").write_text(java)
    (work / "android/widget").mkdir(parents=True)
    (work / "android/widget/Toast.java").write_text('''package android.widget;
public class Toast { public static final int LENGTH_LONG=1; public static int shown; public static String lastMessage;
public static Toast makeText(Object o, CharSequence s, int l) { lastMessage=s.toString(); return new Toast(); }
public void show() { shown++; } }''')
    run([shutil.which("javac") or str(JAVA_BIN / "javac"), "-d", str(work), str(work / "Main.java"), str(work / "android/widget/Toast.java")])
    run([shutil.which("java") or str(JAVA_BIN / "java"), "-ea", "-cp", str(work), "Main", str(work)])
    print("PASS: production texturing/JPEG/App/InitTexturing methods and Main postprocess lambda")
