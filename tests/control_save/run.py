#!/usr/bin/env python3
"""Compile extracted production control/save definitions against labeled host fakes.

No Android/Tango/codec claims: SDK replay and file writes are injected boundaries.
All generated sources/binaries live in a temporary directory; no Gradle build.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def definition(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    # These bounded definitions have no brace-containing string literals.
    for end in range(opening, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError("Unterminated production definition: " + signature)


def compile_test(work, name, source, extra_flags=()):
    output = work / name
    output.with_suffix(".cc").write_text(source)
    command = shlex.split(os.environ.get("CXX", "c++")) + [
        "-std=c++11", "-O1", "-g", "-pthread", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I" + str(ROOT / "common"), *extra_flags,
        str(output.with_suffix(".cc")), "-o", str(output)]
    subprocess.run(command, check=True, timeout=60)
    subprocess.run([str(output)], check=True, timeout=15,
                   env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1"))


def main():
    definitions = {
        "scanner/app/src/main/jni/app.cc": ["void App::OnToggleButtonClicked(", "int App::GetRecoveryState() const",
                                           "bool App::SaveWithTextures("],
        "common/thread/reconstr.cc": ["void Reconstruction::BinderLock()", "bool Reconstruction::BinderTryLock()",
                                     "bool Reconstruction::TryLockFrame(", "void Reconstruction::BinderUnlock()",
                                     "void Reconstruction::SetPhotoMode(", "bool Reconstruction::RecoverScan()"],
        "common/data/image.cc": ["void Image::UpsideDown()"],
    }
    source = (HERE / "boundaries_test.cc.in").read_text().replace(
        "// PRODUCTION_SWAP_NAME", definition("common/data/image.h", "void SwapName(")).replace(
        "// PRODUCTION_DEFINITIONS", "\n\n".join(definition(path, signature)
            for path, signatures in definitions.items() for signature in signatures))
    replay = (HERE / "replay_test.cc.in").read_text().replace(
        "// PRODUCTION_REPLAY", definition("common/tango/scan.cc", "bool TangoScan::RecoverContext("))
    with tempfile.TemporaryDirectory(prefix="scanner-control-save-") as temporary:
        compile_test(Path(temporary), "test", source)
        compile_test(Path(temporary), "replay", replay)


if __name__ == "__main__":
    main()
