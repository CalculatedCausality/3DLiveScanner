#!/usr/bin/env python3
"""Verify immutable native baseline and isolated identity of the quality flavor."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import zipfile

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('apk',type=Path)
parser.add_argument('--sdk',type=Path,default=Path('/tmp/opencode/pixelshare-sdk'))
args=parser.parse_args()
baseline=ROOT/'artifacts/3DLiveScanner-modernized-debug-2026-09-29.apk'
assert hashlib.sha256(baseline.read_bytes()).hexdigest()=='0b1e81ecfcaabcf04494de38ff98754b1ca0e4abf7d99cdfdebb9476b1f51d0e'
with zipfile.ZipFile(baseline) as before,zipfile.ZipFile(args.apk) as after:
    expected={name for name in before.namelist() if name.startswith('lib/') and name.endswith('.so')}
    actual={name for name in after.namelist() if name.startswith('lib/') and name.endswith('.so')}
    assert actual==expected|{'lib/arm64-v8a/libscanner_quality_bridge.so'},(expected,actual)
    hashes={}
    for name in sorted(expected):
        original,candidate=before.read(name),after.read(name)
        assert candidate==original,'Changed baseline native binary: '+name
        hashes[name]=hashlib.sha256(candidate).hexdigest()
metadata=subprocess.check_output([str(args.sdk/'build-tools/35.0.0/aapt'),'dump','badging',str(args.apk)],text=True,timeout=30)
assert "package: name='com.lvonasek.arcore3dscanner.quality'" in metadata
assert "application-label:'3D Scanner Quality Test'" in metadata
print(json.dumps({'apk':str(args.apk),'sha256':hashlib.sha256(args.apk.read_bytes()).hexdigest(),
                  'application_id':'com.lvonasek.arcore3dscanner.quality','immutable_native_libraries':hashes,
                  'scope':'Original capture, rendering, reconstruction and codec binaries unchanged; no 16 KiB runtime claim'},indent=2))
