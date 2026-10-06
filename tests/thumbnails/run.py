#!/usr/bin/env python3
"""Exercise production thumbnail logic with controllable Android boundaries; optionally type-check SDK APIs."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from java_tools import java_command, javac_command

JAVA = java_command()
JAVAC = javac_command()
HERE = Path(__file__).resolve().parent
PRODUCTION = ROOT / "scanner/app/src/main/java/com/lvonasek/arcore3dscanner/ui/ThumbnailLoader.java"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--android-jar", type=Path)
args = parser.parse_args()


with tempfile.TemporaryDirectory(prefix="scanner-thumbnails-") as directory:
    work = Path(directory)
    sources = [*HERE.glob("fakes/**/*.java"), PRODUCTION, HERE / "ThumbnailLoaderTest.java"]
    subprocess.run(JAVAC + ["-source", "8", "-target", "8", "-d", str(work),
                    *map(str, sources)], check=True, timeout=30)
    data = work / "fixtures"
    data.mkdir()
    subprocess.run(JAVA + ["-cp", str(work),
                    "com.lvonasek.arcore3dscanner.ui.ThumbnailLoaderTest", str(data)],
                   check=True, timeout=30)
    if args.android_jar:
        # Only application dependencies are stubbed here; ALL Android types come from the real SDK.
        app_fakes = list(HERE.glob("fakes/com/**/*.java"))
        out = work / "android-types"
        out.mkdir()
        subprocess.run(JAVAC + ["-source", "8", "-target", "8", "-classpath",
                        str(args.android_jar), "-d", str(out), str(PRODUCTION), *map(str, app_fakes)],
                       check=True, timeout=30)
        print("PASS: production ThumbnailLoader type-check against", args.android_jar)
