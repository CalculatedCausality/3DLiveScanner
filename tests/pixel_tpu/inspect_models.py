#!/usr/bin/env python3
"""Download two public candidate artifacts and inspect MiDaS FlatBuffer, without inference.

Stdlib only. The NConv PyTorch checkpoint is hashed, NEVER unpickled/executed.
FlatBuffer field numbers follow tensorflow/lite/schema/schema.fbs (TFL3 v3).
No model source or weights are copied into the scanner repository.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import urllib.request

NCONV_REV = 'd85d4b659f2207b397c62d81f27f363baf3397be'
NCONV_BASE = f'https://raw.githubusercontent.com/abdo-eldesokey/nconv/{NCONV_REV}/'
SOURCES = {
    'nconv-checkpoint.pth.tar': NCONV_BASE + 'workspace/exp_unguided_depth/checkpoints/CNN_ep0003.pth.tar',
    'nconv-LICENSE': NCONV_BASE + 'LICENSE',
    'nconv-network.py.txt': NCONV_BASE + 'workspace/exp_unguided_depth/network_exp_unguided_depth.py',
    'nconv-layer.py.txt': NCONV_BASE + 'modules/nconv.py',
    'nconv-params.json': NCONV_BASE + 'workspace/exp_unguided_depth/params.json',
    'model_opt.tflite': 'https://github.com/isl-org/MiDaS/releases/download/v2_1/model_opt.tflite',
    'midas-LICENSE': 'https://raw.githubusercontent.com/isl-org/MiDaS/v2_1/LICENSE',
}


class Table:
    def __init__(self, data, position):
        self.data, self.position = data, position

    def unpack(self, fmt, pos):
        return struct.unpack_from('<' + fmt, self.data, pos)[0]

    def field(self, index):
        vtable = self.position - self.unpack('i', self.position)
        entry = vtable + 4 + index * 2
        if entry >= vtable + self.unpack('H', vtable):
            return None
        offset = self.unpack('H', entry)
        return self.position + offset if offset else None

    def scalar(self, index, fmt='i', default=0):
        pos = self.field(index)
        return self.unpack(fmt, pos) if pos is not None else default

    def indirect(self, index):
        pos = self.field(index)
        return pos + self.unpack('I', pos) if pos is not None else None

    def table(self, index):
        pos = self.indirect(index)
        return Table(self.data, pos) if pos is not None else None

    def vector(self, index, fmt='i', tables=False):
        pos = self.indirect(index)
        if pos is None:
            return []
        size = 4 if tables else struct.calcsize('<' + fmt)
        count = self.unpack('I', pos)
        if count > len(self.data) // size:
            raise ValueError('Invalid vector size')
        positions = range(pos + 4, pos + 4 + count * size, size)
        return [Table(self.data, p + self.unpack('I', p)) if tables else self.unpack(fmt, p)
                for p in positions]

    def text(self, index):
        pos = self.indirect(index)
        if pos is None:
            return None
        count = self.unpack('I', pos)
        return self.data[pos + 4:pos + 4 + count].decode('utf-8')


def inspect(data):
    if data[4:8] != b'TFL3':
        raise ValueError('Not a TFLite v3 FlatBuffer')
    model = Table(data, struct.unpack_from('<I', data)[0])
    types = {0: 'FLOAT32', 1: 'FLOAT16', 2: 'INT32', 3: 'UINT8', 4: 'INT64', 9: 'INT8'}
    names = {0: 'ADD', 1: 'AVERAGE_POOL_2D', 2: 'CONCATENATION', 3: 'CONV_2D',
             4: 'DEPTHWISE_CONV_2D', 6: 'DEQUANTIZE', 9: 'FULLY_CONNECTED',
             18: 'MUL', 19: 'RELU', 21: 'RELU6', 22: 'RESHAPE',
             23: 'RESIZE_BILINEAR', 34: 'PAD', 40: 'MEAN', 67: 'TRANSPOSE_CONV',
             97: 'RESIZE_NEAREST_NEIGHBOR', 114: 'QUANTIZE'}
    codes = []
    for code in model.vector(1, tables=True):
        builtin = max(code.scalar(0, 'b'), code.scalar(3))
        codes.append(dict(builtin=builtin, name=names.get(builtin, f'BUILTIN_{builtin}'),
                          version=code.scalar(2, default=1), custom=code.text(1)))
    subgraphs = []
    for graph in model.vector(2, tables=True):
        tensors = graph.vector(0, tables=True)

        def tensor_info(i):
            t = tensors[i]
            q = t.table(4)
            return dict(index=i, name=t.text(3), shape=t.vector(0),
                        dtype=types.get(t.scalar(1, 'b'), str(t.scalar(1, 'b'))),
                        scale=q.vector(2, 'f') if q else [],
                        zero_point=q.vector(3, 'q') if q else [])

        counts = Counter(o.scalar(0, 'I') for o in graph.vector(3, tables=True))
        subgraphs.append(dict(inputs=[tensor_info(i) for i in graph.vector(1)],
                              outputs=[tensor_info(i) for i in graph.vector(2)],
                              tensor_types=dict(Counter(types.get(t.scalar(1, 'b'), str(t.scalar(1, 'b')))
                                                        for t in tensors)),
                              tensors_with_quantization_scales=sum(bool(q and q.vector(2, 'f'))
                                                                    for q in [t.table(4) for t in tensors]),
                              operators=[dict(**codes[i], count=n) for i, n in sorted(counts.items())]))
    return dict(version=model.scalar(0, 'I'), description=model.text(3), subgraphs=subgraphs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = {}
    for name, url in SOURCES.items():
        request = urllib.request.Request(url, headers={'User-Agent': 'pixel-tpu-research/1'})
        with urllib.request.urlopen(request, timeout=90) as response:
            data = response.read(100 * 1024 * 1024 + 1)
        if len(data) > 100 * 1024 * 1024:
            raise ValueError('Artifact exceeds bounded download size')
        (args.output / name).write_bytes(data)
        manifest[name] = dict(url=url, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        if name.endswith('.tflite'):
            manifest[name]['flatbuffer'] = inspect(data)
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
