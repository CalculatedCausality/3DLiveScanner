#!/usr/bin/env python3
"""Exact positions/faces/colors comparison, permitting only shading-normal changes."""
import argparse
from pathlib import Path
import struct

def meshes(path):
    with path.open('rb') as stream:
        magic = b'RecordedMeshV1\0'
        assert stream.read(len(magic)) == magic
        count, = struct.unpack('<I', stream.read(4))
        for _ in range(count):
            header = stream.read(40)
            x, y, z, timestamp, nv, nf, cv, cf, flags = struct.unpack('<iiidIIIII', header)
            assert nv <= cv and nf <= cf and flags & ~3 == 0
            positions = stream.read(nv*12)
            if flags & 1:
                stream.seek(nv*12, 1)
            colors = stream.read(nv*4) if flags & 2 else b''
            faces = stream.read(nf*12)
            yield header, positions, colors, faces
        assert not stream.read(1)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('before', type=Path)
parser.add_argument('after', type=Path)
args = parser.parse_args()
a, b = iter(meshes(args.before)), iter(meshes(args.after))
count = 0
while True:
    old, new = next(a, None), next(b, None)
    assert old == new, ('Geometry/metadata differs', count)
    if old is None:
        break
    count += 1
print('Positions, faces, colors and metadata are byte-identical in', count, 'segments; only normals may differ')
