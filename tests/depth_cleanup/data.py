# SPDX-License-Identifier: Apache-2.0
"""Own synthetic metric-depth scenes. No captured data or third-party weights."""
import numpy as np

RESIDUAL_METRES = .02
FEATURE_METRES = .04
INPUT_SCALE = 1 / 128
FAMILIES = ('plane', 'slope', 'curve', 'step', 'thin', 'low_step', 'low_thin', 'holes',
            'outliers', 'high_noise', 'bias', 'clean', 'clean_slope', 'clean_curve')


def windows(a, size=3):
    radius = size // 2
    return np.lib.stride_tricks.sliding_window_view(
        np.pad(a, ((radius, radius), (radius, radius)), mode='edge'), (size, size))


def features(raw, confidence, version='residual'):
    valid = raw > 0
    samples = windows(raw)
    support = samples > 0
    count = support.sum(axis=(-1, -2))
    mean = samples.sum(axis=(-1, -2)) / np.maximum(count, 1)
    residual = np.where(valid, (raw - mean) / FEATURE_METRES, 0)
    if version == 'residual':
        context = valid
    elif version in ('context','wide_context'):
        # A local high-pass residual alone discards the coherent low-frequency
        # shape of a thin edge. Keep a coarse metric context channel while
        # retaining the original fine residual and confidence (zero in holes).
        anchor = float(np.median(raw[valid])) if valid.any() else 0.
        context = np.where(valid,np.clip((raw-anchor)/.25,-1,1),0)
    else:
        raise ValueError('Unknown feature version')
    encoded_residual=residual
    if version=='wide_context':
        wide=windows(raw,7);support_wide=wide>0
        wide_mean=wide.sum(axis=(-1,-2))/np.maximum(support_wide.sum(axis=(-1,-2)),1)
        encoded_residual=np.where(valid,(raw-wide_mean)/FEATURE_METRES,0)
    x = np.stack((np.clip(encoded_residual, -1, 1), context, confidence), axis=-1).astype('float32')
    lo = np.where(support, samples, np.inf).min(axis=(-1, -2))
    hi = samples.max(axis=(-1, -2))
    # No invented measurements, no changes immediately across large depth jumps,
    # and no reliance on padding outside the sensor image.
    eligible = valid & (count == 9) & ((hi - lo) < .08) & ((hi - lo) > 1e-6)
    # If the centre residual is below one half input-quantization code, retain
    # the measured float depth instead of applying a learned DC bias.
    eligible &= np.abs(residual) >= INPUT_SCALE / 2
    eligible[:4] = False
    eligible[-4:] = False
    eligible[:, :4] = False
    eligible[:, -4:] = False
    return x, eligible, mean.astype('float32')


def quantize_input(x):
    return np.clip(np.rint(x / INPUT_SCALE) + 128, 0, 255).astype('uint8')


def postprocess(raw, prediction, eligible):
    correction = np.clip(prediction, -1, 1) * RESIDUAL_METRES
    # Corrections cannot invalidate a positive measurement or create one in a hole.
    out = np.where(eligible, np.maximum(.01, raw + correction), raw)
    return out.astype('float32')


def bilateral(raw, spatial=1.2, range_sigma=.02):
    samples = windows(raw, 5)
    axis = np.arange(-2, 3)
    spatial_weights = np.exp(-(axis[:, None] ** 2 + axis[None, :] ** 2) / (2 * spatial ** 2))
    weights = np.exp(-((samples - raw[..., None, None]) / range_sigma) ** 2 / 2)
    weights *= spatial_weights * (samples > 0)
    out = (samples * weights).sum(axis=(-1, -2)) / np.maximum(weights.sum(axis=(-1, -2)), 1e-9)
    return np.where(raw > 0, out, 0).astype('float32')


def scene(seed, height=120, width=160, family=None, feature_version='residual', include_high_noise=False):
    rng = np.random.default_rng(seed)
    training_families = FAMILIES[:10 if include_high_noise else 9] + ('clean',)
    family = family or training_families[int(rng.integers(0, len(training_families)))]
    yy, xx = np.mgrid[:height, :width]
    rx = (xx - width / 2) / (.9 * width)
    ry = (yy - height / 2) / (.9 * width)
    base = rng.uniform(.65, 4.2)
    nx, ny = rng.uniform(-.25, .25, 2)
    clean = base / (1 + nx * rx + ny * ry)
    if family in ('plane', 'thin', 'low_thin', 'holes', 'outliers', 'high_noise', 'bias', 'clean'):
        clean = np.full((height, width), base)
    if family in ('curve', 'clean_curve'):
        clean += rng.uniform(.2, .8) * (rx ** 2 + ry ** 2)
    foreground = np.zeros((height, width), dtype=bool)
    contrast = 0.
    angle = (0, np.pi/2, rng.uniform(.2, 1.3))[seed % 3]
    coordinate = (xx-width/2)*np.cos(angle) + (yy-height/2)*np.sin(angle)
    center = rng.uniform(-.15,.15)*min(height,width)
    if family in ('step', 'low_step'):
        foreground = coordinate < center
        contrast = .025 if family == 'low_step' else rng.uniform(.15, .7)
        clean += (~foreground) * contrast
    elif family in ('thin', 'low_thin'):
        thickness = int(rng.integers(1, 5))
        foreground = np.abs(coordinate-center) < thickness/2
        contrast = .025 if family == 'low_thin' else rng.uniform(.15, .7)
        clean += (~foreground) * contrast
    sigma = .003 + .0015 * base
    if family == 'high_noise':
        sigma *= 2.5
    noise = rng.normal(0, sigma, clean.shape)
    if family.startswith('clean'):
        noise[:] = 0
    if family == 'bias':
        noise += .02  # An unobservable common-mode error; don't claim to recover it.
    confidence = np.full(clean.shape, .85, dtype='float32')
    outliers = rng.random(clean.shape) < (.025 if family == 'outliers' else .003)
    if family.startswith('clean'):
        outliers[:] = False
    noise[outliers] += rng.choice([-1, 1], outliers.sum()) * rng.uniform(.02, .045, outliers.sum())
    confidence[outliers] = .25
    valid = rng.random(clean.shape) >= .01
    if family == 'holes':
        valid &= ((xx - width * .4) ** 2 + (yy - height * .5) ** 2) > (height * .12) ** 2
        valid[:, width // 2:width // 2 + 3] = False
    raw = np.where(valid, clean + noise, 0).astype('float32')
    confidence[~valid] = 0
    x, eligible, mean = features(raw, confidence, feature_version)
    neighborhood = windows(clean)
    edges = np.ptp(neighborhood, axis=(-1, -2)) > .015
    structure_edges=np.ptp(windows(foreground.astype('uint8')),axis=(-1,-2))>0
    return dict(raw=raw, clean=clean.astype('float32'), confidence=confidence, features=x,
                eligible=eligible, valid=valid, edge=edges, structure_edge=structure_edges, foreground=foreground,
                mean=mean, contrast=contrast, base=base, family=family, seed=seed)


def metrics(sample, output):
    valid = sample['valid']
    error = (output - sample['clean'])[valid].astype('float64')
    edges = sample['edge'] & valid
    interior = valid & ~sample['edge']
    result = dict(rmse_mm=float(np.sqrt(np.mean(error ** 2)) * 1000),
                  mae_mm=float(np.mean(np.abs(error)) * 1000),
                  p95_mm=float(np.quantile(np.abs(error), .95) * 1000),
                  edge_rmse_mm=float(np.sqrt(np.mean((output[edges] - sample['clean'][edges]) ** 2)) * 1000) if edges.any() else None,
                  interior_rmse_mm=float(np.sqrt(np.mean((output[interior] - sample['clean'][interior]) ** 2)) * 1000),
                  invalid_filled=int(np.count_nonzero(output[~valid])),
                  finite=bool(np.isfinite(output).all()))
    if sample['family'] in ('thin', 'low_thin'):
        actual_foreground = output < sample['base'] + sample['contrast'] / 2
        fg, bg = sample['foreground'] & valid, ~sample['foreground'] & valid
        result['thin_recall'] = float(actual_foreground[fg].mean())
        result['thin_false_positive'] = float(actual_foreground[bg].mean())
    return result
