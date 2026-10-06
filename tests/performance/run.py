#!/usr/bin/env python3
"""Host regressions of production scanner code; no downloads or device claims."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
from java_tools import java_command, javac_command


def run(args, timeout=60):
    args = [str(arg) for arg in args]
    print("+ " + shlex.join(args), flush=True)
    subprocess.run(args, check=True, timeout=timeout)


def required_file(path, hint):
    if not path or not Path(path).is_file():
        raise SystemExit(hint)
    return Path(path).resolve()


def java_inputs():
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    android = os.environ.get("ANDROID_JAR")
    if not android and sdk:
        android = Path(sdk) / "platforms" / ("android-" + os.environ.get("ANDROID_API", "33")) / "android.jar"
    return (
        javac_command(), java_command(),
        required_file(android, "Set ANDROID_JAR or ANDROID_SDK_ROOT/ANDROID_HOME (ANDROID_API defaults to 33)."),
        required_file(os.environ.get("JCODEC_JAR"), "Set JCODEC_JAR to org.jcodec:jcodec:0.2.3's jar."),
        required_file(os.environ.get("JCODEC_ANDROID_JAR"), "Set JCODEC_ANDROID_JAR to org.jcodec:jcodec-android:0.2.3's jar."),
    )


def native(work, sanitizers):
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=c++11", "-O1", "-g", "-ffunction-sections", "-fdata-sections", "-DANDROID",
             "-include", HERE / "native/include/host.h"]
    if sanitizers:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for path in [HERE / "native/include", ROOT / "common", ROOT / "third_party/glm",
                 ROOT / "third_party/libpng/include", ROOT / "third_party/libjpeg-turbo/src"]:
        flags += ["-I", path]
    sources = {
        "image": ROOT / "common/data/image.cc",
        "mesh": ROOT / "common/data/mesh.cc",
        "scene": ROOT / "common/thread/scene.cc",
        "codec_init": HERE / "native/codec_init.cc",
    }
    for name, source in sources.items():
        run(cxx + flags + ["-c", source, "-o", work / (name + ".o")], timeout=300)
    for name, objects in [("image_test", ["image", "codec_init"]),
                          ("scene_test", ["image", "mesh", "scene", "codec_init"])]:
        output = work / name
        run(cxx + flags + [HERE / ("native/" + name + ".cc")]
            + [work / (obj + ".o") for obj in objects] + ["-Wl,--gc-sections", "-o", output], timeout=300)
        # UBSan must fail the runner rather than merely print a diagnostic.
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        print("+ " + str(output), flush=True)
        subprocess.run([str(output)], check=True, timeout=30, env=env)


def recorder(work, inputs):
    javac, java, android, jcodec, jcodec_android = inputs
    dirs = {name: work / name for name in ["empty-sourcepath", "production", "android-mocks", "encoder-mock", "tests"]}
    for directory in dirs.values():
        directory.mkdir()
    dependencies = [android, jcodec, jcodec_android]

    def compile_to(output, classpath, sources):
        # Empty sourcepath prevents javac silently choosing mock .java files over real jars.
        run(javac + ["--release", "8", "-sourcepath", dirs["empty-sourcepath"],
             "-cp", os.pathsep.join(map(str, classpath)), "-d", output] + list(sources))

    compile_to(dirs["production"], dependencies, [
        HERE / "java/surface/com/lvonasek/gles/GLESSurfaceView.java",
        ROOT / "common/record/com/lvonasek/record/Recorder.java",
    ])
    compile_to(dirs["android-mocks"], dependencies, sorted((HERE / "java/android-mocks").rglob("*.java")))
    compile_to(dirs["encoder-mock"], dependencies, sorted((HERE / "java/encoder-mock").rglob("*.java")))
    mocked = [dirs["encoder-mock"], dirs["android-mocks"], dirs["production"]] + dependencies
    compile_to(dirs["tests"], mocked, [HERE / "java/RecorderTest.java"])
    # Real codec run physically excludes the fake SequenceEncoder class directory.
    real = [dirs["android-mocks"]] + dependencies
    compile_to(dirs["tests"], real, [HERE / "java/CodecReuseTest.java"])
    for name, classpath in [("RecorderTest", mocked), ("CodecReuseTest", real)]:
        run(java + ["-ea", "-cp", os.pathsep.join(map(str, [dirs["tests"]] + classpath)), name, work], timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", nargs="?", choices=["all", "native", "recorder"], default="all")
    parser.add_argument("--no-sanitizers", action="store_true", help="Explicitly opt out on hosts without ASan/UBSan")
    args = parser.parse_args()
    inputs = java_inputs() if args.suite in ("all", "recorder") else None
    with tempfile.TemporaryDirectory(prefix="scanner-performance-") as temporary:
        work = Path(temporary)
        if args.suite in ("all", "native"):
            native(work, not args.no_sanitizers)
        if inputs:
            recorder(work, inputs)
    print("PASS: requested performance regression suites (host only)")


if __name__ == "__main__":
    main()
