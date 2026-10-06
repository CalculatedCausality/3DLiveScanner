#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Train an original bounded residual CNN and export a fixed NNAPI q8 graph.

Synthetic research only: passing these checks does not establish real-sensor quality.
Model and test payloads stay outside the repository. No scanner/device access.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time

import numpy as np
import torch
from torch import nn
from torch.nn import functional as F
from data import (scene, FAMILIES, INPUT_SCALE, RESIDUAL_METRES, quantize_input,
                  postprocess, bilateral, metrics, windows)

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--steps', type=int, default=1200)
parser.add_argument('--qat-steps', type=int, default=500)
parser.add_argument('--threads', type=int, default=4)
parser.add_argument('--evaluation-seed', type=int, default=9100000)
parser.add_argument('--feature-version', choices=('residual','context','wide_context'), default='residual')
parser.add_argument('--full-frame-crops', action='store_true')
parser.add_argument('--include-high-noise', action='store_true')
parser.add_argument('--edge-weight', type=float, default=3)
parser.add_argument('--foreground-weight', type=float, default=2)
parser.add_argument('--gradient-weight', type=float, default=0)
parser.add_argument('--preserve-training-edges', action='store_true')
parser.add_argument('--strong-baselines', action='store_true')
parser.add_argument('--semantic-edge-loss', action='store_true')
parser.add_argument('--focus-thin', action='store_true')
args = parser.parse_args()
out = args.output.resolve()
assert ROOT not in out.parents and out != ROOT
out.mkdir(exist_ok=False)
assert args.steps >= 1 and args.qat_steps >= 1 and 1 <= args.threads <= 8
torch.set_num_threads(args.threads)
torch.manual_seed(20261001)
torch.use_deterministic_algorithms(True)
np.random.seed(20261001)
start = time.monotonic()
source_hashes={name:hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest() for name in ('train.py','data.py')}
snapshot=out/'sources';snapshot.mkdir()
for name in source_hashes: (snapshot/name).write_bytes(Path(__file__).with_name(name).read_bytes())

# Acceptance criteria are fixed before learning, including a strong CPU baseline.
gates: dict = dict(raw_rmse_improvement_fraction=.10, baseline_rmse_improvement_fraction=.05,
             per_family_max_regression_fraction=.02, per_family_allowance_mm=.1,
             edge_allowance_mm=.1, thin_recall_max_drop=.02,
             invalid_fill_allowed=0, tpu_vs_host_rmse_allowance_mm=.1)
gates['baseline_names'] = ['bilateral_guarded', 'mean_guarded', 'median_guarded']
if args.strong_baselines: gates['baseline_names']+=['mean5_guarded','mean7_guarded']
(out/'acceptance.json').write_text(json.dumps(gates, indent=2)+'\n')

def ste_quantize(x, scale, zero, lo=0, hi=255):
    rounded = torch.clamp(torch.round(x / scale) + zero, lo, hi)
    return x + ((rounded - zero) * scale - x).detach()

class Residual(nn.Module):
    def __init__(self):
        super().__init__()
        self.layers = nn.ModuleList([nn.Conv2d(3,12,3,padding=1), nn.Conv2d(12,12,3,padding=1), nn.Conv2d(12,1,3,padding=1)])
        nn.init.zeros_(self.layers[-1].weight)
        nn.init.zeros_(self.layers[-1].bias)
        self.scales = [INPUT_SCALE, 1/128, 1/128, 1/128]
        self.zeros = [128, 0, 0, 128]
    def forward(self, x, quant=False, maxima=None):
        if quant:
            x = ste_quantize(x, self.scales[0], self.zeros[0])
        for i, layer in enumerate(self.layers):
            w, b = layer.weight, layer.bias
            if quant:
                ws = max(float(w.detach().abs().max()) / 127, 1e-8)
                w = ste_quantize(w, ws, 128)
                bs = self.scales[i] * ws
                b = b + (torch.round(b / bs) * bs - b).detach()
            x = F.conv2d(x, w, b, padding=1)
            if i < 2:
                x = F.relu(x)
                if maxima is not None:
                    maxima[i] = max(maxima[i], float(x.max()))
            if quant:
                x = ste_quantize(x, self.scales[i+1], self.zeros[i+1])
        return x

model = Residual()
optimizer = torch.optim.Adam(model.parameters(), lr=.001)
def train(steps, seed_offset, quant):
    pool=[]
    if args.full_frame_crops:
        pool=[scene(seed_offset+i,feature_version=args.feature_version,include_high_noise=args.include_high_noise) for i in range(128)]
    for step in range(steps):
        if pool:
            rng=np.random.default_rng(seed_offset+6000000+step)
            if step%100==0:
                for j in range(16):
                    at=(step//100*16+j)%len(pool)
                    pool[at]=scene(seed_offset+128+step*8+j,feature_version=args.feature_version,include_high_noise=args.include_high_noise)
            samples=[]
            for j in range(8):
                sample=pool[int(rng.integers(len(pool)))]
                if args.focus_thin and j<4:
                    desired=('low_thin','thin','low_step','step')[j]
                    choices=[s for s in pool if s['family']==desired]
                    sample=choices[int(rng.integers(len(choices)))] if choices else scene(seed_offset+900000+step*8+j,family=desired,feature_version=args.feature_version)
                y=int(rng.integers(120-48+1)); x=int(rng.integers(160-48+1))
                if args.focus_thin and j<4:
                    locations=np.argwhere(sample['foreground'] if sample['family'] in ('thin','low_thin') else sample['structure_edge'])
                    if len(locations):
                        py,px=locations[int(rng.integers(len(locations)))]
                        y=int(np.clip(py-24+int(rng.integers(-8,9)),0,120-48))
                        x=int(np.clip(px-24+int(rng.integers(-8,9)),0,160-48))
                crop={field:sample[field][y:y+48,x:x+48].copy() for field in ('raw','clean','features','eligible','edge','structure_edge','foreground')}
                crop['family']=sample['family']
                crop['eligible'][:4]=False;crop['eligible'][-4:]=False
                crop['eligible'][:,:4]=False;crop['eligible'][:,-4:]=False
                samples.append(crop)
        else:
            samples = [scene(seed_offset + step*8+j,48,48,feature_version=args.feature_version,include_high_noise=args.include_high_noise) for j in range(8)]
        x = torch.from_numpy(np.stack([s['features'].transpose(2,0,1) for s in samples]))
        target = torch.from_numpy(np.stack([(s['clean']-s['raw'])/RESIDUAL_METRES for s in samples]))[:,None]
        target = target.clamp(-1,1)
        edge_key='structure_edge' if args.semantic_edge_loss else 'edge'
        if args.preserve_training_edges:
            protected=torch.from_numpy(np.stack([s[edge_key] | (s['foreground'] if s['family'] in ('thin','low_thin') else False) for s in samples]))[:,None]
            target=torch.where(protected,torch.zeros_like(target),target)
        mask = torch.from_numpy(np.stack([s['eligible'] for s in samples]))[:,None]
        weights = torch.from_numpy(np.stack([1+args.edge_weight*s[edge_key]+args.foreground_weight*(s['foreground'] if not args.focus_thin or s['family'] in ('thin','low_thin') else False) for s in samples]))[:,None]
        prediction = model(x, quant)
        loss = (((prediction-target).clamp(-3,3)**2)*mask*weights).sum() / (mask*weights).sum().clamp(min=1)
        if args.gradient_weight:
            difference=prediction-target
            for axis in (2,3):
                left=[slice(None)]*4;right=[slice(None)]*4
                left[axis]=slice(None,-1);right[axis]=slice(1,None)
                l,r=tuple(left),tuple(right)
                pair=mask[l]&mask[r]
                pair_weight=torch.maximum(weights[l],weights[r])*pair
                gradient=(difference[l]-difference[r]).clamp(-3,3)
                loss=loss+args.gradient_weight*(gradient.square()*pair_weight).sum()/pair_weight.sum().clamp(min=1)
        optimizer.zero_grad();loss.backward();torch.nn.utils.clip_grad_norm_(model.parameters(),1.0);optimizer.step()
        if step % 100 == 0 or step+1 == steps:
            print(json.dumps(dict(phase='qat' if quant else 'float', step=step+1, loss=float(loss), elapsed=time.monotonic()-start)),flush=True)

train(args.steps,1000000,False)
float_weights = {k:v.detach().clone() for k,v in model.state_dict().items()}
maxima = [0.,0.]
with torch.no_grad():
    for i in range(48):
        s=scene(3000000+i,120 if args.full_frame_crops else 64,160 if args.full_frame_crops else 64,feature_version=args.feature_version,include_high_noise=args.include_high_noise)
        model(torch.from_numpy(s['features'].transpose(2,0,1)[None]), maxima=maxima)
for i, maximum in enumerate(maxima):
    # Full calibration maxima plus 10% headroom; no test-set calibration.
    model.scales[i+1] = float(np.float32(max(maximum*1.1, .01)/255))
for group in optimizer.param_groups: group['lr']=.0002
train(args.qat_steps,4000000,True)

def export():
    payload = bytearray(b'DCLN0001')
    for scale,zero in zip(model.scales,model.zeros): payload.extend(struct.pack('<fi',scale,zero))
    parameters={}
    for i,layer in enumerate(model.layers):
        w=layer.weight.detach().numpy()
        ws=np.float32(max(float(np.abs(w).max())/127,1e-8))
        q=np.clip(np.rint(w/ws)+128,0,255).astype('uint8').transpose(0,2,3,1).copy()
        bias=np.rint(layer.bias.detach().numpy()/np.float32(model.scales[i]*ws)).astype('<i4')
        payload.extend(struct.pack('<f',ws));payload.extend(q.tobytes());payload.extend(bias.tobytes())
        parameters[f'layer{i}_weight']=w;parameters[f'layer{i}_bias']=layer.bias.detach().numpy()
    (out/'model.bin').write_bytes(payload)
    np.savez(out/'trained_float_parameters.npz',**parameters)
    return hashlib.sha256(payload).hexdigest()

digest=export()
float_model=Residual();float_model.load_state_dict(float_weights)
rows=[];data={};inputs=[]
with torch.no_grad():
    for i,family in enumerate(FAMILIES):
        for repeat in range(3):
            key=f'{family}-{repeat}'
            sample=scene(args.evaluation_seed+i*99+repeat,120,160,family,feature_version=args.feature_version)
            x=torch.from_numpy(sample['features'].transpose(2,0,1)[None])
            fp=float_model(x)[0,0].numpy()
            quant=model(x,True)[0,0].numpy()
            median=np.median(windows(sample['raw']),axis=(-1,-2)).astype('float32')
            filtered=bilateral(sample['raw'])
            variants=dict(raw=sample['raw'], bilateral=filtered,
                          bilateral_guarded=postprocess(sample['raw'],(filtered-sample['raw'])/RESIDUAL_METRES,sample['eligible']),
                          mean_guarded=postprocess(sample['raw'],(sample['mean']-sample['raw'])/RESIDUAL_METRES,sample['eligible']),
                          median_guarded=postprocess(sample['raw'],(median-sample['raw'])/RESIDUAL_METRES,sample['eligible']),
                          float_model=postprocess(sample['raw'],fp,sample['eligible']),
                          quant_model=postprocess(sample['raw'],quant,sample['eligible']))
            for size in (5,7):
                neighbours=windows(sample['raw'],size)
                average=neighbours.sum(axis=(-1,-2))/np.maximum((neighbours>0).sum(axis=(-1,-2)),1)
                variants[f'mean{size}_guarded']=postprocess(sample['raw'],(average-sample['raw'])/RESIDUAL_METRES,sample['eligible'])
            rows.append(dict(key=key, family=family, seed=sample['seed'], metrics={name:metrics(sample,value) for name,value in variants.items()}))
            for field in ('raw','clean','confidence','eligible','valid','edge','foreground'):
                data[key+'__'+field]=sample[field]
            data[key+'__quant_prediction']=quant
            data[key+'__base']=np.array(sample['base']);data[key+'__contrast']=np.array(sample['contrast'])
            inputs.append(quantize_input(sample['features']))
            assert np.array_equal(variants['quant_model'][~sample['eligible']],sample['raw'][~sample['eligible']])
np.savez_compressed(out/'evaluation.npz',**data)
(out/'inputs.bin').write_bytes(np.stack(inputs).tobytes())
report=dict(seed=20261001, train_steps=args.steps, qat_steps=args.qat_steps, training_domain='own synthetic depth scenes only',
            evaluation_seed=args.evaluation_seed,
            feature_version=args.feature_version,full_frame_crops=args.full_frame_crops,
            include_high_noise=args.include_high_noise,edge_weight=args.edge_weight,
            foreground_weight=args.foreground_weight,gradient_weight=args.gradient_weight,
            preserve_training_edges=args.preserve_training_edges,strong_baselines=args.strong_baselines,
            semantic_edge_loss=args.semantic_edge_loss,
            focus_thin=args.focus_thin,
            parameters=sum(p.numel() for p in model.parameters()), architecture='3x3 conv 3->12 ReLU, 12->12 ReLU, 12->1 linear; stride1 SAME',
            activation_scales=model.scales, activation_zeros=model.zeros, model_sha256=digest,
            model_bytes=(out/'model.bin').stat().st_size, residual_limit_metres=RESIDUAL_METRES,
            training_seconds=time.monotonic()-start, torch=torch.__version__, numpy=np.__version__,
            source_hashes=source_hashes,
            license='Original code, generated training scenes and trained parameters: Apache-2.0; no third-party pretrained weights',
            physical_accuracy_claim=False, acceptance=gates, evaluations=rows)
assert all(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()==value for name,value in source_hashes.items()), 'Training source changed during run'
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(dict(report=str(out/'results.json'),model_sha256=digest,model_bytes=report['model_bytes'],evaluations=len(rows))),flush=True)
