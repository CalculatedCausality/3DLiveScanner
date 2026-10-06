"""Shared Java tool discovery for host GL and original-engine boundary checks."""
import os
from pathlib import Path
import shutil
import subprocess


def java_command():
    home = os.environ.get("JAVA_HOME")
    executable = str(Path(home) / "bin/java") if home else shutil.which("java")
    if not executable or not Path(executable).is_file():
        raise SystemExit("Java is required; set JAVA_HOME or add java to PATH.")
    return [executable]


def javac_command():
    home = os.environ.get("JAVA_HOME")
    executable = str(Path(home) / "bin/javac") if home else shutil.which("javac")
    if executable and Path(executable).is_file():
        return [executable]
    # Some environments include the compiler module but omit the javac launcher.
    command = java_command() + ["-m", "jdk.compiler/com.sun.tools.javac.Main"]
    result = subprocess.run(command + ["-version"], capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise SystemExit("Java compilation is required; install a JDK or set JAVA_HOME.")
    return command


def jni_home():
    home = Path(os.environ["JAVA_HOME"]) if os.environ.get("JAVA_HOME") else Path(java_command()[0]).resolve().parents[1]
    required = (home / "include/jni.h", home / "include/linux/jni_md.h")
    if not all(path.is_file() for path in required):
        raise SystemExit("The JNI bridge check requires Linux JDK headers; set JAVA_HOME to a complete JDK.")
    return home
