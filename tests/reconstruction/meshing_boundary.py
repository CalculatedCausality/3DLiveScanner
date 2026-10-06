#!/usr/bin/env python3
"""Exact chunk-local open-edge comparison for recorded A/B mesh artifacts."""
import argparse
from collections import Counter
import math
from pathlib import Path
import struct

def read_mesh(path):
    with path.open('rb') as stream:
        magic = b'RecordedMeshV1\0'
        assert stream.read(len(magic)) == magic
        count, = struct.unpack('<I', stream.read(4))
        for _ in range(count):
            x, y, z, timestamp, nv, nf, cv, cf, flags = struct.unpack('<iiidIIIII', stream.read(40))
            assert nv <= cv and nf <= cf and flags & ~3 == 0
            points = list(struct.iter_unpack('<fff', stream.read(nv*12)))
            stream.seek(nv*((12 if flags & 1 else 0)+(4 if flags & 2 else 0)), 1)
            uses, winding = Counter(), Counter()
            for a, b, c in struct.iter_unpack('<III', stream.read(nf*12)):
                for u, v in ((a,b),(b,c),(c,a)):
                    edge = tuple(sorted((points[u],points[v])))
                    uses[edge] += 1
                    winding[edge] += 1 if points[u] < points[v] else -1
            yield (x,y,z), {edge: (n, winding[edge]) for edge, n in uses.items() if n != 2 or winding[edge]}
        assert not stream.read(1)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('before', type=Path)
parser.add_argument('after', type=Path)
parser.add_argument('--cache', type=Path)
args = parser.parse_args()
old = dict(read_mesh(args.before)); new = dict(read_mesh(args.after))
assert old.keys() == new.keys()
differing = []
for key in old:
    if old[key] != new[key]:
        edges = {edge for edge in old[key].keys() | new[key].keys() if old[key].get(edge) != new[key].get(edge)}
        print('chunk', key, 'changed open edges', sorted(edges))
        differing.extend(edges)
if differing and args.cache:
    with args.cache.open('rb') as stream:
        magic, h, count, accepted, rejected, samples = struct.unpack('<IdIIII', stream.read(28))
        assert magic == 0x4d455331
        chunks = {}
        for _ in range(count):
            key = struct.unpack('<iii',stream.read(12)); chunks[key] = stream.read(98304)
    cells = set(tuple(math.floor((a[j]+b[j])*.5/h) for j in range(3)) for a,b in differing)
    for cell in sorted(cells):
        values = []
        for j in range(8):
            p = tuple(cell[k]+((j>>k)&1) for k in range(3))
            chunk = tuple(t//16 for t in p)
            offset = (p[0]%16)+16*(p[1]%16)+256*(p[2]%16)
            values.append(struct.unpack_from('<f',chunks[chunk],24*offset)[0])
        print('cell',cell,'h',h,'sdf',values)
assert not differing, 'Changed chunk-local boundary'
print('Exact chunk-local open-edge geometry/counts match in',len(old),'segments')
