#!/usr/bin/env python3
"""Standalone real NDK/SDK API compile, no device and no Gradle output directories."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", "/tmp/opencode/android-sdk"))
NDK = Path(os.environ.get("ANDROID_NDK_HOME", str(SDK / "ndk/28.2.13676358")))
JAVA_BIN = Path(os.environ.get("JAVA_HOME", "/tmp/opencode/scanner-jdk17")) / "bin"
compiler = NDK / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++"
assert compiler.is_file(), "Set ANDROID_NDK_HOME to an installed NDK"
includes = ["common", "reconstruction", "third_party/glm", "third_party/tango_3d_reconstruction/include",
            "third_party/libjpeg-turbo/src", "third_party/libjpeg-turbo/include", "third_party/libpng/include",
            "third_party/opencv/include", "third_party/delaunay", "arcore/include-modern"]
for unit in ["common/arcore/arcore.cc", "common/arcore/service.cc", "common/tango/scan.cc", "common/tango/texturize.cc", "common/data/image.cc", "common/thread/reconstr.cc", "scanner/app/src/main/jni/app.cc"]:
    subprocess.run([str(compiler), "-std=c++11", "-fsyntax-only", "-DANDROID", "-DSCANNER_MODERN=1",
                    *["-I" + str(ROOT / i) for i in includes], str(ROOT / unit)], cwd=ROOT, check=True, timeout=180)
    print("PASS real NDK headers:", unit, flush=True)
for unit in ["common/tango/scan.cc", "common/tango/texturize.cc"]:
    subprocess.run([str(compiler), "-std=c++11", "-fsyntax-only", "-DANDROID", "-DSCANNER_MODERN=0",
                    *["-I" + str(ROOT / i) for i in includes], str(ROOT / unit)],
                   cwd=ROOT, check=True, timeout=180)
    print("PASS real NDK headers: legacy", unit, flush=True)

with tempfile.TemporaryDirectory(prefix="texturing-sdk-", dir="/tmp/opencode") as directory:
    work = Path(directory)
    java = ROOT / "scanner/app/src/main/java/com/lvonasek/arcore3dscanner/main/JNI.java"
    # Only generated R constants are stand-ins; Context/Resources/JNI declaration are real SDK/source.
    names = sorted(set(re.findall(r"R\.string\.(\w+)", java.read_text())))
    resource = work / "R.java"
    resource.write_text("package com.lvonasek.arcore3dscanner; public final class R { public static final class string {"
                        + "".join("public static final int " + name + "=" + str(i) + ";" for i, name in enumerate(names)) + "}}")
    build_config=work/'BuildConfig.java'
    build_config.write_text('package com.lvonasek.arcore3dscanner; public final class BuildConfig { public static final String FLAVOR="modern"; }')
    subprocess.run([shutil.which("javac") or str(JAVA_BIN / "javac"), "-classpath", str(SDK / "platforms/android-35/android.jar"), "-d", str(work),
                    "-h", str(work), str(resource), str(build_config), str(java)], cwd=ROOT, check=True, timeout=60)
    generated = (work / "com_lvonasek_arcore3dscanner_main_JNI.h").read_text()
    assert "JNIEXPORT jboolean JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_texturize" in generated
    connected = re.search(r"JNIEXPORT jboolean JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_onARServiceConnected\s*\([^;]*\);", generated)
    assert connected, "Missing generated capture/paging JNI declaration"
    textured = re.search(r"JNIEXPORT jboolean JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_texturize\s*\([^;]*\);", generated)
    error_getter = re.search(r"JNIEXPORT jstring JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_getTexturingError\s*\([^;]*\);", generated)
    assert textured and error_getter, "Missing generated export JNI declarations"
    depth_setter = re.search(r"JNIEXPORT void JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_setExperimentalDepth\s*\([^;]*\);", generated)
    depth_status = re.search(r"JNIEXPORT jstring JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_getExperimentalDepthStatus\s*\([^;]*\);", generated)
    assert depth_setter and depth_status, "Missing generated experimental-depth JNI declarations"
    coverage_setter=re.search(r"JNIEXPORT void JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_setCoveragePreview\s*\([^;]*\);",generated)
    finish_capture=re.search(r"JNIEXPORT jboolean JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_finishCapture\s*\([^;]*\);",generated)
    assert coverage_setter and finish_capture,"Missing capture-first JNI contracts"
    contract = work / "capture_jni.h"
    contract.write_text('#include <jni.h>\nextern "C" {\n' + '\n'.join(
        declaration.group() for declaration in (connected, textured, error_getter, depth_setter, depth_status, coverage_setter, finish_capture)) + '\n}\n')
    subprocess.run([str(compiler), "-std=c++11", "-fsyntax-only", "-DANDROID", "-DSCANNER_MODERN=1",
                    "-include", str(contract), *["-I" + str(ROOT / i) for i in includes],
                    str(ROOT / "scanner/app/src/main/jni/app.cc")], cwd=ROOT, check=True, timeout=180)
    print("PASS generated Java/native capture and export-result JNI contracts", flush=True)
    aapt = sorted((SDK / "build-tools").glob("*/aapt2"))[-1]
    subprocess.run([str(aapt), "compile", "-o", str(work),
                    str(ROOT / "scanner/app/src/main/res/values/texturing_strings.xml")], cwd=ROOT, check=True, timeout=60)
    print("PASS real SDK: JNI.java / generated boolean JNI signature / texturing error resource")
