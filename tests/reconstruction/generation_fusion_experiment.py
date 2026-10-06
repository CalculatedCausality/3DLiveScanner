#!/usr/bin/env python3
"""Build a fusion experiment from a frozen core without editing production code."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--baseline', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--mode', choices=('batch', 'axial-dot'), default='batch')
args = parser.parse_args()
out = args.output.resolve()
if out == ROOT or ROOT in out.parents:
    parser.error('Experimental sources must stay outside the workspace')
out.mkdir(exist_ok=False)
before, after = out / 'before', out / 'candidate'
before.mkdir(); after.mkdir()
records = {}
for name in ('core.cc', 'paging.h', 'paging_store.h'):
    data = (args.baseline / name).read_bytes()
    (before / name).write_bytes(data)
    (after / name).write_bytes(data)
    records['reconstruction/' + name] = hashlib.sha256(data).hexdigest()
api = ROOT / 'third_party/tango_3d_reconstruction/include/tango_3d_reconstruction_api.h'
shutil.copyfile(api, before / api.name)
records[str(api.relative_to(ROOT))] = hashlib.sha256(api.read_bytes()).hexdigest()
(before / 'manifest.json').write_text(json.dumps(dict(files=records, scope='frozen production baseline'), indent=2))
source = (after / 'core.cc').read_text()
start = source.index('void integrate(Transaction& tx,')
opening = source.index('{', start)
depth = 0
end = None
for i in range(opening, len(source)):
    depth += (source[i] == '{') - (source[i] == '}')
    if depth == 0:
        end = i + 1
        break
assert end is not None
if args.mode == 'batch':
    replacement = (ROOT / 'tests/reconstruction/generation_fusion.cc.in').read_text()
else:
    replacement = source[start:end]
    marker = 'glm::dvec3 ray = camera.q * (p / range);'
    assert marker in replacement and 'camera.local(position(n)*h).z' in replacement
    replacement = replacement.replace(marker, marker + '\n    const glm::dvec3 cameraForward = camera.q * glm::dvec3(0,0,1);')
    replacement = replacement.replace('camera.local(position(n)*h).z',
                                      'glm::dot(cameraForward, position(n)*h-camera.t)')
(after / 'core.cc').write_text(source[:start] + replacement + source[end:])
print(json.dumps(dict(mode=args.mode, baseline=str(before), candidate=str(after / 'core.cc'),
    before_sha256=records['reconstruction/core.cc'],
    candidate_sha256=hashlib.sha256((after / 'core.cc').read_bytes()).hexdigest()), indent=2))
