#!/usr/bin/env python3
"""Regression for model-container lookup; requires Python 3 and a JDK 8+."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from java_tools import java_command, javac_command


with tempfile.TemporaryDirectory(prefix="scanner-lookup-") as directory:
    work = Path(directory)
    fixtures = work / "fixtures"
    fixtures.mkdir()
    subprocess.run([*javac_command(), "-source", "8", "-target", "8", "-d", str(work),
                    str(ROOT / "scanner/app/src/main/java/com/lvonasek/arcore3dscanner/ui/ModelLookup.java"),
                    str(ROOT / "scanner/app/src/main/java/com/lvonasek/arcore3dscanner/ui/StorageRoot.java"),
                    str(ROOT / "scanner/app/src/main/java/com/lvonasek/arcore3dscanner/ui/CaptureWorkspace.java"),
                    str(ROOT / "tests/model_lookup/ModelLookupTest.java"),
                    str(ROOT / "tests/model_lookup/StorageRootTest.java"),
                    str(ROOT / "tests/model_lookup/CaptureWorkspaceTest.java")], check=True, timeout=30)
    subprocess.run([*java_command(), "-cp", str(work),
                    "com.lvonasek.arcore3dscanner.ui.ModelLookupTest", str(fixtures)], check=True, timeout=15)
    subprocess.run([*java_command(), "-cp", str(work),
                    "com.lvonasek.arcore3dscanner.ui.StorageRootTest", str(fixtures)], check=True, timeout=15)
    subprocess.run([*java_command(), "-cp", str(work),
                    "com.lvonasek.arcore3dscanner.ui.CaptureWorkspaceTest", str(fixtures)], check=True, timeout=15)
