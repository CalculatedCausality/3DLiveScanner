# SPDX-License-Identifier: Apache-2.0
"""Independent integer-accumulation reference for the exported NNAPI model."""
import struct
from pathlib import Path
import numpy as np

class QuantizedModel:
    def __init__(self, path):
        data=Path(path).read_bytes()
        if len(data)!=1880 or data[:8]!=b'DCLN0001':
            raise ValueError('Invalid DCLN0001 model')
        self.scales=[];self.zeros=[];self.layers=[];offset=8
        for _ in range(4):
            scale,zero=struct.unpack_from('<fi',data,offset);offset+=8
            if not np.isfinite(scale) or scale<=0 or not 0<=zero<=255: raise ValueError('Invalid activation quantization')
            self.scales.append(scale);self.zeros.append(zero)
        for oc,ic in ((12,3),(12,12),(1,12)):
            scale=struct.unpack_from('<f',data,offset)[0];offset+=4
            if not np.isfinite(scale) or scale<=0: raise ValueError('Invalid weight scale')
            count=oc*3*3*ic
            weight=np.frombuffer(data,dtype='uint8',count=count,offset=offset).reshape(oc,3,3,ic).astype('int32')-128
            offset+=count
            bias=np.frombuffer(data,dtype='<i4',count=oc,offset=offset).astype('int64');offset+=oc*4
            self.layers.append((scale,weight,bias))
        assert offset==len(data)

    def codes(self, input_codes):
        x=np.asarray(input_codes,dtype='uint8')
        if x.ndim!=3 or x.shape[-1]!=3: raise ValueError('Expected HWC three-channel input')
        height,width=x.shape[:2]
        for i,(weight_scale,weights,bias) in enumerate(self.layers):
            padded=np.pad(x,((1,1),(1,1),(0,0)),constant_values=self.zeros[i])
            patches=np.lib.stride_tricks.sliding_window_view(padded,(3,3),axis=(0,1))
            columns=patches.transpose(0,1,3,4,2).reshape(height*width,-1).astype('int32')-self.zeros[i]
            # Dot products contain at most 108 terms and fit int32; add biases
            # in int64 so malformed extreme bias values cannot wrap silently.
            accumulator=(columns @ weights.reshape(weights.shape[0],-1).T).astype('int64')+bias
            if i<2: accumulator=np.maximum(accumulator,0)
            scale=float(np.float32(self.scales[i]*weight_scale))/self.scales[i+1]
            x=np.clip(np.rint(accumulator*scale)+self.zeros[i+1],0,255).astype('uint8').reshape(height,width,-1)
        return x

    def predict(self, input_codes):
        return (self.codes(input_codes)[...,0].astype('float32')-self.zeros[-1])*self.scales[-1]
