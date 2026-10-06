#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Geometry-preservation contracts, independent of learned parameters."""
import numpy as np
from data import features, postprocess, metrics, scene, windows, quantize_input

h,w=120,160
raw=np.full((h,w),2.,dtype='float32')
raw[40:50,70:75]=0
raw[:,100:102]=1
confidence=(raw>0).astype('float32')
x,eligible,_=features(raw,confidence)
assert np.isfinite(x).all()
assert not eligible.any(), 'Perfect constant surfaces/strong thin steps should be unchanged'
for prediction in (-100.,100.):
    result=postprocess(raw,np.full((h,w),prediction,dtype='float32'),eligible)
    assert np.array_equal(result,raw)

rng=np.random.default_rng(781)
raw=np.where(raw>0,raw+rng.normal(0,.008,raw.shape),0).astype('float32')
x,eligible,_=features(raw,confidence)
assert eligible.any()
result=postprocess(raw,np.full((h,w),100.,dtype='float32'),eligible)
assert np.isfinite(result).all() and not np.count_nonzero(result[raw==0])
assert np.max(np.abs(result-raw)) <= .020001
assert np.array_equal(result[~eligible],raw[~eligible])

for family in ('clean','clean_slope','clean_curve'):
    sample=scene(9300000,120,160,family)
    assert not sample['eligible'].any(), 'Quantization-invisible clean-surface detail must retain original floats'

# Verify that the evaluator notices a sub-voxel strip being flattened. Merely
# checking output point counts/validity would miss this loss of real detail.
truth=np.full((h,w),2.025,dtype='float32');truth[:,80]=2
foreground=truth==2
sample=dict(clean=truth,valid=np.ones((h,w),bool),
            edge=np.ptp(windows(truth),axis=(-1,-2))>.015,
            family='low_thin',foreground=foreground,base=2.,contrast=.025)
bad=truth.copy();bad[foreground]+=.02
assert metrics(sample,truth)['thin_recall']==1
assert metrics(sample,bad)['thin_recall']==0

# The revised encoder retains a coarse foreground/background signal, without
# changing validity or the shared correction policy used for comparisons.
confidence=np.ones_like(truth)
old,old_mask,_=features(truth,confidence,'residual')
context,new_mask,_=features(truth,confidence,'context')
assert np.array_equal(old_mask,new_mask)
q=quantize_input(context)
assert q[60,80,1] < q[60,90,1]-10
assert np.array_equal(context[...,0],old[...,0])
assert np.array_equal(context[...,2],old[...,2])
for family in ('slope','curve','clean_slope','clean_curve'):
    sample=scene(9300010,120,160,family)
    assert not sample['structure_edge'].any(), 'Smooth metric gradients are not discontinuities'
sample=scene(9300011,120,160,'low_step')
assert 0 < np.count_nonzero(sample['structure_edge']) < h*w/4
print('PASS holes, bounded corrections, protected pixels, noiseless planes/curves and thin-feature regression detection')
