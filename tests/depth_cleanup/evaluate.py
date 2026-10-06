#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fresh evaluation of frozen exported weights; no optimization or calibration."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from data import scene,FAMILIES,RESIDUAL_METRES,windows,bilateral,metrics,quantize_input,postprocess
from inference import QuantizedModel

parser=argparse.ArgumentParser()
parser.add_argument('--training',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--seed',type=int,required=True)
parser.add_argument('--repeats',type=int,default=3)
args=parser.parse_args()
root=Path(__file__).resolve().parents[2];out=args.output.resolve()
assert root!=out and root not in out.parents and 1<=args.repeats<=4
out.mkdir(exist_ok=False)
original=json.loads((args.training/'results.json').read_text())
model_bytes=(args.training/'model.bin').read_bytes()
assert hashlib.sha256(model_bytes).hexdigest()==original['model_sha256']
(out/'model.bin').write_bytes(model_bytes)
model=QuantizedModel(out/'model.bin')
rows=[];data={};inputs=[]
for i,family in enumerate(FAMILIES):
    for repeat in range(args.repeats):
        key=f'{family}-{repeat}'
        sample=scene(args.seed+i*99+repeat,120,160,family,feature_version=original.get('feature_version','residual'))
        codes=quantize_input(sample['features']);prediction=model.predict(codes)
        filtered=bilateral(sample['raw']);median=np.median(windows(sample['raw']),axis=(-1,-2)).astype('float32')
        variants=dict(raw=sample['raw'],bilateral=filtered,
                      bilateral_guarded=postprocess(sample['raw'],(filtered-sample['raw'])/RESIDUAL_METRES,sample['eligible']),
                      mean_guarded=postprocess(sample['raw'],(sample['mean']-sample['raw'])/RESIDUAL_METRES,sample['eligible']),
                      median_guarded=postprocess(sample['raw'],(median-sample['raw'])/RESIDUAL_METRES,sample['eligible']),
                      quant_model=postprocess(sample['raw'],prediction,sample['eligible']))
        for size in (5,7):
            neighbours=windows(sample['raw'],size)
            average=neighbours.sum(axis=(-1,-2))/np.maximum((neighbours>0).sum(axis=(-1,-2)),1)
            variants[f'mean{size}_guarded']=postprocess(sample['raw'],(average-sample['raw'])/RESIDUAL_METRES,sample['eligible'])
        # Diagnostics only, never available to inference: show the floor imposed
        # by the unchanged-pixel policy and residual bound. A local
        # translation-invariant model cannot identify a common-mode depth bias.
        variants['oracle_guarded']=postprocess(sample['raw'],(sample['clean']-sample['raw'])/RESIDUAL_METRES,sample['eligible'])
        reference=sample['clean']+(.02 if family=='bias' else 0)
        variants['oracle_bias_preserving']=postprocess(sample['raw'],(reference-sample['raw'])/RESIDUAL_METRES,sample['eligible'])
        rows.append(dict(key=key,family=family,seed=sample['seed'],metrics={name:metrics(sample,value) for name,value in variants.items()}))
        for field in ('raw','clean','confidence','eligible','valid','edge','foreground'):
            data[key+'__'+field]=sample[field]
        data[key+'__quant_prediction']=prediction
        data[key+'__base']=np.array(sample['base']);data[key+'__contrast']=np.array(sample['contrast'])
        inputs.append(codes)
        print(key,'RMSE mm:',rows[-1]['metrics']['quant_model']['rmse_mm'],flush=True)
np.savez_compressed(out/'evaluation.npz',**data)
(out/'inputs.bin').write_bytes(np.stack(inputs).tobytes())
report=dict(original,evaluations=rows,evaluation_seed=args.seed,
            evaluation_kind='frozen exported weights; integer accumulation; no fitting',
            parent_training=str(args.training),
            evaluation_source_hashes={name:hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest() for name in ('data.py','evaluate.py','inference.py')})
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
(out/'acceptance.json').write_text(json.dumps(report['acceptance'],indent=2)+'\n')
print('Frozen-weight evaluation:',out/'results.json')
