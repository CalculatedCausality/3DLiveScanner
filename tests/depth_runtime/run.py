#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host contracts plus byte-exact NumPy WIDE_CONTEXT preprocessing oracle."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix='depth-runtime-', dir='/tmp/opencode'))
source = (ROOT/'common/depth/model_data.h').read_text()
model = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', source))
assert len(model) == 1880 and model[:8] == b'DCLN0001'
assert hashlib.sha256(model).hexdigest() == '76ef7a8c8376a1a921e2f055419d27b4eaeca425ac2b430dd3f14a89eae84be0'
exe = OUT/'runtime_test'
flags = ['-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread']
if os.environ.get('SANITIZE'):
    flags += ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-I'+str(ROOT/'third_party/glm'),
                str(ROOT/'tests/depth_runtime/runtime_test.cc'), '-ldl', '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
spec = importlib.util.spec_from_file_location('data', ROOT/'tests/depth_cleanup/data.py')
assert spec is not None and spec.loader is not None
data = importlib.util.module_from_spec(spec)
spec.loader.exec_module(data)
cases = 0
def compare(raw, conf, label):
    h, w = raw.shape
    x, eligible, _ = data.features(raw, conf, version='wide_context')
    fixture = OUT/'input.bin'
    fixture.write_bytes(np.array([w,h],dtype='<i4').tobytes()+raw.astype('<f4').tobytes()+conf.astype('<f4').tobytes())
    output = OUT/'features.bin'
    subprocess.run([str(exe), 'features', str(fixture), str(output)], check=True)
    actual = np.frombuffer(output.read_bytes(),dtype=np.uint8)
    expected = np.concatenate((data.quantize_input(x).ravel(),eligible.astype(np.uint8).ravel()))
    diff = np.flatnonzero(actual != expected)
    assert not len(diff), (w,h,label,len(diff),[(int(i),int(actual[i]),int(expected[i])) for i in diff[:10]])

for h, w in [(1, 1), (7, 8), (11, 13), (120, 160), (37, 61), (128, 96)]:
    for seed, family in enumerate(data.FAMILIES):
        sample = data.scene(seed, height=h, width=w, family=family, feature_version='wide_context')
        compare(sample['raw'], sample['confidence'], family)
        cases += 1
for threshold in [np.float32(.08), np.float32(1e-6)]:
    for step in [np.nextafter(threshold,np.float32(0)),threshold,np.nextafter(threshold,np.float32(1))]:
        raw = np.full((11,13),2**-24,dtype=np.float32)
        raw[5,6] += step
        compare(raw,np.full_like(raw,.5),f'range-threshold-{step}')
        cases += 1
print(f'Model SHA256 verified; {cases} byte-exact NumPy feature/mask cases passed; artifacts: {OUT}')
