#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Evaluate held-out synthetic depth and optional actual NNAPI outputs."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from data import metrics, postprocess

parser=argparse.ArgumentParser()
parser.add_argument('--training',type=Path,required=True)
parser.add_argument('--tpu',type=Path)
parser.add_argument('--cpu',type=Path)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--brief',action='store_true')
args=parser.parse_args()
report=json.loads((args.training/'results.json').read_text())
assert hashlib.sha256((args.training/'model.bin').read_bytes()).hexdigest()==report['model_sha256'], 'Training model changed'
data=np.load(args.training/'evaluation.npz',allow_pickle=False)
rows=report['evaluations']
raw_outputs={}
for name,path in (('tpu',args.tpu),('nnapi_cpu',args.cpu)):
    if path:
        manifest=json.loads((path.parent/'results.json').read_text())
        assert manifest['status']=='success', 'Device execution did not succeed'
        assert manifest['files']['model.bin']['sha256']==report['model_sha256'], 'Wrong device model'
        assert manifest['files']['inputs.bin']['sha256']==hashlib.sha256((args.training/'inputs.bin').read_bytes()).hexdigest(), 'Wrong device inputs'
        assert manifest['files'][path.name]['sha256']==hashlib.sha256(path.read_bytes()).hexdigest(), 'Device outputs changed'
        values=np.fromfile(path,dtype='uint8')
        assert values.size==len(rows)*120*160
        raw_outputs[name]=values.reshape(len(rows),120,160)

comparisons={name:[] for name in raw_outputs}
for index,row in enumerate(rows):
    key=row['key']
    sample={field:data[key+'__'+field] for field in ('raw','clean','confidence','eligible','valid','edge','foreground')}
    sample.update(family=row['family'],base=float(data[key+'__base']),contrast=float(data[key+'__contrast']))
    host=postprocess(sample['raw'],data[key+'__quant_prediction'],sample['eligible'])
    for name,values in raw_outputs.items():
        prediction=(values[index].astype('float32')-report['activation_zeros'][-1])*report['activation_scales'][-1]
        actual=postprocess(sample['raw'],prediction,sample['eligible'])
        row['metrics'][name]=metrics(sample,actual)
        delta=(actual-host)[sample['valid']]
        comparisons[name].append(dict(rmse_mm=float(np.sqrt(np.mean(delta**2))*1000),max_mm=float(np.abs(delta).max()*1000)))
        assert np.array_equal(actual[~sample['eligible']],sample['raw'][~sample['eligible']])

variants=list(rows[0]['metrics'])
def aggregate(items):
    return {name:float(np.sqrt(np.mean([item['metrics'][name]['rmse_mm']**2 for item in items]))) for name in variants}
overall=aggregate(rows)
families={family:aggregate([row for row in rows if row['family']==family]) for family in sorted({r['family'] for r in rows})}
candidate='tpu' if args.tpu else 'quant_model'
rules=report['acceptance']
best=min(rules['baseline_names'],key=lambda name:overall[name])
failures=[]
failure_details=[]
if overall[candidate] > overall['raw']*(1-rules['raw_rmse_improvement_fraction']): failures.append('Less than required improvement over raw depth')
if overall[candidate] > overall[best]*(1-rules['baseline_rmse_improvement_fraction']): failures.append('Does not beat the strongest matched-policy CPU filter by 5%')
for family,values in families.items():
    if values[candidate]>values['raw']*(1+rules['per_family_max_regression_fraction'])+rules['per_family_allowance_mm']:
        failures.append(f'{family}: depth RMSE regression')
for row in rows:
    measured,raw=row['metrics'][candidate],row['metrics']['raw']
    if not measured['finite'] or measured['invalid_filled']:
        failures.append(row['key']+': nonfinite output or invented depth')
    if raw['edge_rmse_mm'] is not None and measured['edge_rmse_mm']>raw['edge_rmse_mm']*(1+rules['per_family_max_regression_fraction'])+rules['edge_allowance_mm']:
        failures.append(row['key']+': edge error regression')
        failure_details.append(dict(scene=row['key'],metric='edge_rmse_mm',raw=raw['edge_rmse_mm'],candidate=measured['edge_rmse_mm'],
                                    allowed=raw['edge_rmse_mm']*(1+rules['per_family_max_regression_fraction'])+rules['edge_allowance_mm']))
    if 'thin_recall' in raw and measured['thin_recall']<raw['thin_recall']-rules['thin_recall_max_drop']:
        failures.append(row['key']+': thin-surface recall regression')
if args.tpu and max(v['rmse_mm'] for v in comparisons['tpu'])>rules['tpu_vs_host_rmse_allowance_mm']:
    failures.append('TPU output differs too much from host quantized simulation')

def detail_failures(name):
    found=[]
    for family,values in families.items():
        if values[name]>values['raw']*(1+rules['per_family_max_regression_fraction'])+rules['per_family_allowance_mm']:
            found.append(f'{family}: depth RMSE')
    for row in rows:
        value,raw=row['metrics'][name],row['metrics']['raw']
        if not value['finite'] or value['invalid_filled']: found.append(row['key']+': invalid output')
        if raw['edge_rmse_mm'] is not None and value['edge_rmse_mm']>raw['edge_rmse_mm']*(1+rules['per_family_max_regression_fraction'])+rules['edge_allowance_mm']:
            found.append(row['key']+': edge RMSE')
        if 'thin_recall' in raw and value['thin_recall']<raw['thin_recall']-rules['thin_recall_max_drop']:
            found.append(row['key']+': thin recall')
    return found

details={name:detail_failures(name) for name in ['raw','bilateral',*rules['baseline_names'],candidate]}
safe=[name for name in rules['baseline_names'] if not details[name]]
safe_reference=min(safe,key=lambda name:overall[name]) if safe else 'raw'

summary=dict(candidate=candidate, model_sha256=report['model_sha256'],evaluated_frames=len(rows),
             weighting='Equal weight per held-out scene, root mean squared scene RMSE',
             overall_rmse_mm=overall,per_family_rmse_mm=families,best_matched_baseline=best,
             raw_improvement_percent=100*(1-overall[candidate]/overall['raw']),
             baseline_improvement_percent=100*(1-overall[candidate]/overall[best]),
             device_vs_host=comparisons,acceptance_passed=not failures,failures=failures,
             failure_details=failure_details,
             detail_failures_by_method=details,detail_preservation_passed=not details[candidate],
             detail_safe_reference=safe_reference,
             detail_safe_reference_improvement_percent=100*(1-overall[candidate]/overall[safe_reference]),
             real_sensor_validation=False,app_integration_permitted=False,
             scope='Trained synthetic-depth research; even a synthetic pass requires separate real-sensor validation',
             analysis_source_hashes={name:hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest() for name in ('analyze.py','data.py')},
             output_sha256={name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in (('tpu',args.tpu),('nnapi_cpu',args.cpu)) if path},
             evaluations=rows)
with args.output.open('x') as target: json.dump(summary,target,indent=2);target.write('\n')
hidden={'evaluations','device_vs_host'}
if args.brief: hidden.add('per_family_rmse_mm')
print(json.dumps({k:v for k,v in summary.items() if k not in hidden},indent=2))
