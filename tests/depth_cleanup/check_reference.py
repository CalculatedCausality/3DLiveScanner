#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the independent integer reference against retained actual NNAPI data."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from inference import QuantizedModel

p=argparse.ArgumentParser();p.add_argument('--device-run',type=Path,required=True)
p.add_argument('--device',choices=('both','tpu','cpu'),default='both')
args=p.parse_args()
manifest=json.loads((args.device_run/'results.json').read_text())
assert manifest['status']=='success'
for name in ('model.bin','inputs.bin','depth.cpu.bin','depth.tpu.bin'):
    assert hashlib.sha256((args.device_run/name).read_bytes()).hexdigest()==manifest['files'][name]['sha256']
model=QuantizedModel(args.device_run/'model.bin')
inputs=np.fromfile(args.device_run/'inputs.bin',dtype='uint8').reshape(-1,120,160,3)
cpu=np.fromfile(args.device_run/'depth.cpu.bin',dtype='uint8').reshape(-1,120,160,1)
tpu=np.fromfile(args.device_run/'depth.tpu.bin',dtype='uint8').reshape(cpu.shape)
worst_cpu=worst_tpu=0
for i in range(len(inputs)):
    reference=model.codes(inputs[i]).astype('int16')
    dc=int(np.max(np.abs(reference-cpu[i].astype('int16'))))
    dt=int(np.max(np.abs(reference-tpu[i].astype('int16'))))
    worst_cpu=max(worst_cpu,dc);worst_tpu=max(worst_tpu,dt)
passed=(args.device=='cpu' or worst_tpu<=2) and (args.device=='tpu' or worst_cpu<=2)
print(json.dumps(dict(frames=len(inputs),maximum_code_difference_cpu=worst_cpu,maximum_code_difference_tpu=worst_tpu,checked_device=args.device,pass_reference_check=passed)),flush=True)
assert passed,(worst_cpu,worst_tpu)
