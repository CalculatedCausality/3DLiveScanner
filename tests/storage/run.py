#!/usr/bin/env python3
"""Compile real IO/Exporter sources; Android/UI stubs are boundary-only.

--baseline runs the legacy-API regressions against git HEAD (expected to fail).
No downloads or global installs; all build products go into a temporary directory.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))
from java_tools import java_command, javac_command
SOURCES = [
    "common/utils/com/lvonasek/utils/IO.java",
    "scanner/app/src/main/java/com/lvonasek/arcore3dscanner/main/Exporter.java",
]


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--host-only", action="store_true", help="Explicitly omit Android jar type-checking")
    args = parser.parse_args()
    javac, java = javac_command(), java_command()
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    android = os.environ.get("ANDROID_JAR") or (Path(sdk) / "platforms/android-33/android.jar" if sdk else None)
    if not args.host_only and (not android or not Path(android).is_file()):
        parser.error("Set ANDROID_JAR or ANDROID_SDK_ROOT/ANDROID_HOME, or explicitly use --host-only")
    with tempfile.TemporaryDirectory(prefix="storage-tests-", dir="/tmp/opencode") as temporary:
        build = Path(temporary)
        production = [ROOT / source for source in SOURCES]
        if args.baseline:
            production = []
            for source in SOURCES:
                target = build / Path(source).name
                target.write_bytes(subprocess.check_output(["git", "show", "HEAD:" + source], cwd=ROOT, timeout=15))
                production.append(target)
        if not args.host_only:
            run(*javac, "--release", "8", "-classpath", android, "-d", build / "android",
                *production, HERE / "stubs/com/lvonasek/arcore3dscanner/ui/AbstractActivity.java")
        classes = build / "host"
        tests = ["LegacyRegression"] if args.baseline else ["LegacyRegression", "StorageRegression", "DirectoryPublicationTest"]
        run(*javac, "--release", "8", "-d", classes, *production, *HERE.glob("stubs/**/*.java"),
            HERE / "TestSupport.java", *(HERE / (name + ".java") for name in tests))
        for name in tests:
            run(*java, "-ea", "-Djava.io.tmpdir=" + str(build), "-cp", classes, name)


if __name__ == "__main__":
    main()
