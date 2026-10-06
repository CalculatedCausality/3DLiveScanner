#!/usr/bin/env python3
"""Fuse frozen geometry once, then A/B extraction on identical cached voxels."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import recorded

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--fixture', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cache', type=Path, required=True)
parser.add_argument('--cache-report', type=Path, help='Provenance report for a cache made before sidecar metadata support')
parser.add_argument('--core-source', type=Path, default=ROOT / 'reconstruction/core.cc')
parser.add_argument('--resolution', type=float, choices=(.02, .04), required=True)
args = parser.parse_args()
manifest = recorded.validate(args.fixture)
cache_meta = args.cache.with_suffix(args.cache.suffix + '.json')
cache_before = recorded.sha_file(args.cache) if args.cache.exists() else None
cap = 4096 if args.resolution == .02 else 1024
if cache_before:
    if not cache_meta.exists() and not args.cache_report:
        parser.error('An existing cache requires its metadata sidecar or --cache-report')
    provenance = json.loads((cache_meta if cache_meta.exists() else args.cache_report).read_text())
    assert provenance['cache_sha256'] == cache_before
    assert provenance['fixture_sha256'] == manifest['fixture_sha256']
    assert provenance['resolution'] == args.resolution and provenance.get('max_chunks', 1024) == cap
sources = {str(args.core_source): recorded.sha_file(args.core_source)}
for header in args.core_source.parent.glob('*.h'):
    sources[str(header)] = recorded.sha_file(header)
args.output.mkdir(exist_ok=False)
adapter = args.output / 'pose_adapter.cc'
adapter.write_text('#include <glm/glm.hpp>\n#include <glm/gtc/quaternion.hpp>\n#include <tango_3d_reconstruction_api.h>\n'
                   'Tango3DR_Pose recordedPose(glm::mat4 matrix) ' + recorded.production_pose() + '\n')
flags = ['-std=c++11', '-O2', '-g', '-DANDROID', '-ffunction-sections', '-fdata-sections', '-fno-fast-math',
         '-DMESHING_CORE_SOURCE="' + str(args.core_source.resolve()) + '"',
         '-include', str(ROOT / 'tests/performance/native/include/host.h')]
includes = ['common', 'reconstruction', 'third_party/glm', 'third_party/tango_3d_reconstruction/include',
            'tests/performance/native/include', 'tests/reconstruction']
binary = args.output / 'meshing_recorded'
command = shlex.split(os.environ.get('CXX', 'g++')) + flags + ['-I' + str(ROOT / p) for p in includes] + [
    str(ROOT / 'tests/reconstruction/meshing_recorded.cc'), str(ROOT / 'common/data/dataset.cc'), str(adapter),
    '-Wl,--gc-sections', '-pthread', '-o', str(binary)]
subprocess.run(command, check=True)
mesh = args.output / 'ordered_mesh.bin'
run = subprocess.run([str(binary), str(args.fixture), str(args.cache), str(args.resolution), str(mesh)],
                     check=True, capture_output=True, text=True)
result = json.loads(run.stdout)
result['diagnostics'] = run.stderr.splitlines()
assert recorded.validate(args.fixture) == manifest
assert all(recorded.sha_file(Path(path)) == digest for path, digest in sources.items())
assert not cache_before or recorded.sha_file(args.cache) == cache_before
result.update(fixture_sha256=manifest['fixture_sha256'], cache_sha256=recorded.sha_file(args.cache),
              core_sha256=recorded.sha_file(args.core_source), mesh_sha256=recorded.sha_file(mesh))
result['source_hashes'] = sources
if (args.core_source.parent / 'meshing.h').exists():
    result['meshing_sha256'] = recorded.sha_file(args.core_source.parent / 'meshing.h')
if not cache_meta.exists():
    metadata = {key: result[key] for key in ('fixture_sha256', 'cache_sha256', 'resolution', 'max_chunks')}
    cache_meta.write_text(json.dumps(metadata, indent=2) + '\n')
(args.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
