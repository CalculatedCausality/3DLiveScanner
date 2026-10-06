#!/usr/bin/env python3
"""Localize cross-device numerical disagreement; this is NOT a quality benchmark."""
import argparse
import hashlib
import json
from pathlib import Path
from typing import Any
import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    report: dict[str, Any] = {'interpretation': 'Cross-device numerical agreement only; no trained model or quality claim.'}
    for case in ('l2', 'decoder', 'decoder_l2'):
        values, hashes = {}, {}
        for device in ('cpu', 'tpu'):
            data = (args.directory / f'{case}-q8.{device}.bin').read_bytes()
            hashes[device] = hashlib.sha256(data).hexdigest()
            values[device] = np.frombuffer(data, dtype=np.uint8).astype(np.int16).reshape(120, 160, 3)
        diff = np.abs(values['cpu'] - values['tpu'])
        bad = np.any(diff > 2, axis=2)
        yy, xx = np.where(bad)
        result = dict(sha256=hashes, max_code_difference=int(diff.max()),
                      components_above_two_codes=int(np.sum(diff > 2)),
                      pixels_above_two_codes=int(bad.sum()),
                      bad_pixel_bounds_xyxy=[int(xx.min()), int(yy.min()), int(xx.max()), int(yy.max())]
                      if len(xx) else None)
        if case == 'decoder_l2':
            # The separately compiled pre-L2 graph is a diagnostic, not proof of
            # the intermediate values chosen by the fused compiler.
            pre = np.frombuffer((args.directory / 'decoder-q8.cpu.bin').read_bytes(),
                                dtype=np.uint8).astype(float).reshape(120, 160, 3) - 128
            magnitude = np.linalg.norm(pre, axis=2)
            result['pre_l2_reference_code_norm_at_bad_pixels'] = dict(
                min=float(magnitude[bad].min()), max=float(magnitude[bad].max())) if bad.any() else None
            a, b = values['cpu'].astype(float) - 128, values['tpu'].astype(float) - 128
            denominator = np.linalg.norm(a, axis=2) * np.linalg.norm(b, axis=2)
            valid = denominator > 0
            angles = np.degrees(np.arccos(np.clip(np.sum(a*b, axis=2)[valid] / denominator[valid], -1, 1)))
            result['cross_device_angle_degrees_not_quality'] = dict(
                max=float(angles.max()), p95=float(np.percentile(angles, 95)),
                mean=float(angles.mean()), nonzero_pairs=int(valid.sum()))
        report[case] = result
    (args.directory / 'numerical-analysis.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
