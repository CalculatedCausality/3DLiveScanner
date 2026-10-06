#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Frozen-source synthetic preprocessing comparison; never invokes adb or the app."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--freeze', action='store_true')
parser.add_argument('--baseline', type=Path)
parser.add_argument('--android-ndk', type=Path)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()

def hashes(directory):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(directory.iterdir()) if p.suffix in ('.cc', '.h')}

if args.freeze:
    out = Path(tempfile.mkdtemp(prefix='depth-preprocess-before-', dir='/tmp/opencode'))
    for p in (ROOT/'common/depth').iterdir():
        if p.suffix in ('.cc', '.h'):
            shutil.copy2(p, out/p.name)
    (out/'sha256.json').write_text(json.dumps(hashes(out), indent=2)+'\n')
    print(out)
    print((out/'sha256.json').read_text())
    raise SystemExit(0)

assert args.baseline, '--baseline must identify a pre-edit --freeze directory'
assert hashes(args.baseline) == json.loads((args.baseline/'sha256.json').read_text())
# This comparison is deliberately scoped to the preprocessing-only optimization.
before = (args.baseline/'experimental.cc').read_text()
after = (ROOT/'common/depth/experimental.cc').read_text()
assert before.split('Features Preprocess(')[0] == after.split('Features Preprocess(')[0]
assert before.split('bool SamePoint(')[1] == after.split('bool SamePoint(')[1]
for name in ['experimental.h', 'model_data.h']:
    assert (args.baseline/name).read_bytes() == (ROOT/'common/depth'/name).read_bytes()
out = Path(tempfile.mkdtemp(prefix='depth-preprocess-perf-', dir='/tmp/opencode'))
print('Artifacts:', out, flush=True)
print('Baseline:', json.dumps(hashes(args.baseline)), flush=True)
print('Current:', json.dumps(hashes(ROOT/'common/depth')), flush=True)
(out/'sources.json').write_text(json.dumps({'baseline': hashes(args.baseline),
                                          'current': hashes(ROOT/'common/depth')}, indent=2)+'\n')

# Reuse the existing private fake NNAPI and helpers without editing runtime_test.cc.
# Its entry point is not run by the benchmark (the normal suite runs separately).
test = (ROOT/'tests/depth_runtime/runtime_test.cc').read_text()
test = test.replace('int main(', 'int ContractMain(')
end = test.rfind('}')
test = test[:end] + '    return 0;\n' + test[end:]
compiler = os.environ.get('CXX', 'g++')
tools = None
flags = ['-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
         '-I'+str(ROOT/'third_party/glm')]
libs = ['-ldl']
if args.android_ndk:
    tools = args.android_ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    compiler = str(tools/'aarch64-linux-android24-clang++')
    libs += ['-llog', '-static-libstdc++', '-Wl,-z,max-page-size=16384']
if args.sanitize:
    assert not args.android_ndk
    flags += ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
for name, directory in [('baseline', args.baseline), ('current', ROOT/'common/depth')]:
    inc = out/(name+'_runtime.inc')
    inc.write_text(test.replace('../../common/depth/experimental.cc', str(directory.resolve()/'experimental.cc')))
    subprocess.run([compiler, *flags, '-DRUNTIME_TEST_SOURCE="'+str(inc)+'"',
                    str(ROOT/'tests/depth_runtime/performance.cc'), *libs,
                    '-o', str(out/name)], check=True)
    if args.android_ndk:
        assert tools is not None
        dynamic = subprocess.check_output([str(tools/'llvm-readelf'), '-d', str(out/name)], text=True)
        symbols = subprocess.check_output([str(tools/'llvm-nm'), '-u', str(out/name)], text=True)
        assert 'libneuralnetworks' not in dynamic and 'ANeuralNetworks' not in symbols

# The original 90 NumPy-oracle inputs, followed by deterministic generated cases.
import numpy as np
spec = importlib.util.spec_from_file_location('data', ROOT/'tests/depth_cleanup/data.py')
assert spec is not None and spec.loader is not None
data = importlib.util.module_from_spec(spec)
spec.loader.exec_module(data)
cases = []
for h, w in [(1, 1), (7, 8), (11, 13), (120, 160), (37, 61), (128, 96)]:
    for seed, family in enumerate(data.FAMILIES):
        sample = data.scene(seed, height=h, width=w, family=family, feature_version='wide_context')
        cases.append((sample['raw'], sample['confidence']))
for threshold in [np.float32(.08), np.float32(1e-6)]:
    for step in [np.nextafter(threshold, np.float32(0)), threshold, np.nextafter(threshold, np.float32(1))]:
        raw = np.full((11, 13), 2**-24, dtype=np.float32)
        raw[5, 6] += step
        cases.append((raw, np.full_like(raw, .5)))
assert len(cases) == 90
rng = np.random.default_rng(20261001)
shapes = [(1, 1024), (1024, 1), (2, 257), (257, 2), (3, 3), (4, 7),
          (8, 9), (9, 8), (90, 160), (120, 160), (240, 320),
          (480, 320), (512, 512), (256, 1024), (1024, 256)]
shapes += [(int(rng.integers(1, 65)), int(rng.integers(1, 129))) for _ in range(40)]
for h, w in shapes:
    for mode in range(4):
        raw = (1 + rng.uniform(-.025, .025, (h, w))).astype('f4')
        if mode == 1:
            raw[rng.random((h, w)) < .3] = 0
        elif mode == 2:
            values = np.array([0, -0., np.nextafter(np.float32(0), np.float32(1)),
                               1e-6, .05, .08, 10000, np.nextafter(np.float32(10000), np.float32(0))], dtype='f4')
            raw = rng.choice(values, (h, w))
        elif mode == 3:
            raw = rng.uniform(0, 10000, (h, w)).astype('f4')
        conf = rng.integers(0, 257, (h, w)).astype('f4')/256
        cases.append((raw, conf))
corpus = out/'corpus.bin'
with corpus.open('wb') as stream:
    for raw, conf in cases:
        h, w = raw.shape
        stream.write(np.array([w, h], dtype='<i4').tobytes())
        stream.write(raw.astype('<f4').tobytes())
        stream.write(conf.astype('<f4').tobytes())
print(f'Corpus: {len(cases)} cases (including original 90)', flush=True)
if args.android_ndk:
    print('API24 native binaries built; no mandatory NNAPI dependency. Run manually with bench or compare arguments.')
else:
    for name in ['baseline', 'current']:
        with (out/(name+'-compare.log')).open('w') as log:
            subprocess.run([str(out/name), 'compare', str(corpus), str(out/(name+'.bin'))],
                           check=True, stderr=log)
    a, b = (out/'baseline.bin').read_bytes(), (out/'current.bin').read_bytes()
    assert a == b, 'Feature/mask/full-point/stats byte comparison failed'
    print(f'PASS: all {len(a)} serialized feature/mask/point/stats bytes identical; SHA256 {hashlib.sha256(a).hexdigest()}', flush=True)
    if not args.sanitize:
        records = []
        for index, name in enumerate(['baseline', 'current', 'current', 'baseline']):
            print('ABBA component timing:', name, flush=True)
            text = subprocess.check_output([str(out/name), 'bench', '8', str(out/(name+'-bench.bin'))], text=True)
            (out/(str(index)+'-'+name+'-bench.log')).write_text(text)
            print(text, end='', flush=True)
            records.append(text)
        assert (out/'baseline-bench.bin').read_bytes() == (out/'current-bench.bin').read_bytes()
        print('PASS: every benchmark feature/mask byte matches baseline', flush=True)
        parsed = [dict((shape+' '+kind, float(ms)) for shape, kind, ms in re.findall(
            r'(\d+x\d+) (dense|holes) preprocessing_component_ms=([0-9.]+)', text)) for text in records]
        for label in parsed[0]:
            baseline = (parsed[0][label]+parsed[3][label])/2
            current = (parsed[1][label]+parsed[2][label])/2
            print(f'{label}: ABBA mean-of-medians baseline={baseline:.6f}ms current={current:.6f}ms '
                  f'delta={current-baseline:.6f}ms reduction={100*(1-current/baseline):.2f}% speedup={baseline/current:.3f}x')
