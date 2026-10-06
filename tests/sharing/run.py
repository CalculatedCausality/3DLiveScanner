#!/usr/bin/env python3
"""Run the dependency-free packaging tests with JDK 17, leaving no repo artifacts."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests"))
from java_tools import java_command, javac_command

compiler = javac_command()
with tempfile.TemporaryDirectory(prefix="scanner-sharing-") as output:
    source = root / "scanner/app/src/main/java/com/lvonasek/arcore3dscanner/sharing/ModelPackage.java"
    test = root / "tests/sharing/ModelPackageTest.java"
    subprocess.run(compiler + ["-d", output, str(source), str(test)], check=True)
    subprocess.run(java_command() + ["-ea", "-cp", output,
                    "com.lvonasek.arcore3dscanner.sharing.ModelPackageTest"], check=True)
