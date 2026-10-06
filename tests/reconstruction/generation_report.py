#!/usr/bin/env python3
"""Summarize generated-workload/analytic comparisons without hiding quality regressions."""
import argparse
import json
from pathlib import Path
import statistics
from typing import Any

parser = argparse.ArgumentParser()
parser.add_argument('report', type=Path)
parser.add_argument('--after', type=Path, help='Compare two host analytic reports')
parser.add_argument('--brief', action='store_true')
args = parser.parse_args()
first = json.loads(args.report.read_text())
timing_comparable = True
if args.after:
    second = json.loads(args.after.read_text())
    assert first['resolution'] == second['resolution']
    timing_comparable = (first.get('sanitizer') == second.get('sanitizer')
                         and first.get('flags') == second.get('flags'))
    for filename in ('generation_quality.cc', 'recorded_geometry.h'):
        fixture_hashes = [next(value for path, value in report['source_hashes'].items()
                              if Path(path).name == filename) for report in (first, second)]
        assert fixture_hashes[0] == fixture_hashes[1], 'Different analytic fixture: ' + filename
    variants = dict(before=first['results'], after=second['results'])
else:
    variants = first['results']
assert isinstance(variants, dict) and 'before' in variants and 'after' in variants
before, after = variants['before'], variants['after']

def median_metric(runs, index, metric):
    return statistics.median(row[index][metric] for row in runs)

def change(a, b):
    return None if a == 0 else (b/a-1)*100

output = []
for i in range(len(before[0])):
    a, b = before[0][i], after[0][i]
    row: dict[str, Any]
    if isinstance(a['update_ms'], list):
        row = dict(scope='native generated workload, not camera FPS')
        for name, runs in variants.items():
            updates = [t for run in runs for t in run[i]['update_ms']]
            extracts = [t for run in runs for t in run[i]['extract_ms']]
            row[name] = dict(update_median_ms=statistics.median(updates),
                update_mean_ms=statistics.mean(updates),
                extract_median_ms=statistics.median(extracts),
                sequence_median_ms=statistics.median(sum(run[i]['update_ms'])+sum(run[i]['extract_ms']) for run in runs),
                faces=runs[0][i]['faces'], segments=runs[0][i]['segments'])
        row['update_change_percent'] = change(row['before']['update_mean_ms'], row['after']['update_mean_ms'])
        row['total_change_percent'] = change(row['before']['sequence_median_ms'], row['after']['sequence_median_ms'])
    else:
        assert a['scene'] == b['scene'] and a['observations'] == b['observations']
        row = dict(scene=a['scene'])
        for key in ('faces', 'vertices', 'payload_bytes', 'surface_rms_m', 'surface_max_m',
                    'coverage_rms_m', 'coverage_p95_m', 'coverage_within_voxel_fraction',
                    'face_normal_mean_deg', 'shading_normal_mean_deg', 'seam_normal_p95_deg',
                    'triangle_quality_area_mean', 'skinny_area_fraction', 'degenerate_faces',
                    'nonmanifold_edges', 'winding_errors', 'boundary_edges', 'gap_bridge_faces'):
            row[key] = dict(before=a[key], after=b[key], change_percent=change(a[key], b[key]))
        for metric in ('update_ms', 'extract_ms'):
            row[metric] = dict(before=median_metric(before,i,metric), after=median_metric(after,i,metric))
            row[metric]['comparable'] = timing_comparable
            row[metric]['change_percent'] = change(row[metric]['before'],row[metric]['after']) if timing_comparable else None
    output.append(row)
if args.brief:
    for row in output:
        if 'scene' not in row:
            print(json.dumps(row))
            continue
        print(row['scene'],
              'faces=%d->%d' % (row['faces']['before'],row['faces']['after']),
              ('extract_change=%.1f%%' % row['extract_ms']['change_percent']) if timing_comparable
              else 'extract_change=n/a(different-build-flags)',
              'surface_rms_mm=%.4f->%.4f' % (row['surface_rms_m']['before']*1000,row['surface_rms_m']['after']*1000),
              'coverage=%.4f->%.4f' % (row['coverage_within_voxel_fraction']['before'],row['coverage_within_voxel_fraction']['after']),
              'shading_deg=%.2f->%.2f' % (row['shading_normal_mean_deg']['before'],row['shading_normal_mean_deg']['after']),
              'seam_p95_deg=%.2f->%.2f' % (row['seam_normal_p95_deg']['before'],row['seam_normal_p95_deg']['after']),
              'nonmanifold=%d->%d' % (row['nonmanifold_edges']['before'],row['nonmanifold_edges']['after']),
               'gap_bridges=%d->%d' % (row['gap_bridge_faces']['before'],row['gap_bridge_faces']['after']))
    if output and timing_comparable and all('scene' in row for row in output):
        totals = {}
        for name, runs in variants.items():
            # Sum the fixed scene suite within each repeat before taking its
            # median; percentages are not averaged across differently sized scenes.
            totals[name] = {metric: statistics.median(sum(row[metric] for row in run) for run in runs)
                            for metric in ('update_ms', 'extract_ms')}
            totals[name]['sequence_ms'] = statistics.median(
                sum(row['update_ms'] + row['extract_ms'] for row in run) for run in runs)
        print('Fixed-suite totals:', json.dumps(totals))
else:
    print(json.dumps(output, indent=2, allow_nan=False))
