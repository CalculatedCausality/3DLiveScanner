#!/usr/bin/env python3
"""Compile/run private field-normal and stencil invalidation checks."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT / 'reconstruction/core.cc')
parser.add_argument('--before', action='store_true')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
if not args.before:
    def body(text):
        begin = text.index('{', text.index('void integrate('))
        depth = 1
        end = begin + 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        return text[begin:end]
    actual = body((args.source.parent / 'ray_fusion.h').read_text()).replace(
        '\n    if (!tx.context.pager) { integrateDirect(tx,point,camera,image,color_pose); return; }', '')
    expected = body((ROOT / 'tests/reconstruction/generation_fusion.cc.in').read_text())
    assert actual == expected, 'Paged fusion must retain the validated prototype body'
with tempfile.TemporaryDirectory(prefix='meshing-normals-', dir='/tmp/opencode') as temp:
    work = Path(temp); cache = work / 'cache'; cache.mkdir()
    flags = ['-std=c++11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-DRECONSTRUCTION_CORE_TESTING',
             '-DMESHING_CORE_SOURCE="' + str(args.source.resolve()) + '"']
    if args.before:
        flags += ['-DMESHING_BEFORE_NORMALS']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie']
    binary = work / 'normals'
    subprocess.run([os.environ.get('CXX', 'g++')] + flags + [
        '-I' + str(ROOT / 'third_party/glm'), '-I' + str(ROOT / 'third_party/tango_3d_reconstruction/include'),
        str(ROOT / 'tests/reconstruction/meshing_normals.cc'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(cache)], check=True, env=dict(os.environ,
        ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
    assert not list(cache.iterdir())
