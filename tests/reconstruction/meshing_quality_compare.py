#!/usr/bin/env python3
"""Read independent generation-quality reports; require unchanged geometry."""
import argparse
import json
import statistics
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('before', type=Path)
parser.add_argument('after', type=Path)
args = parser.parse_args()
before = json.loads((args.before / 'results.json').read_text())['results']
after = json.loads((args.after / 'results.json').read_text())['results']
a, b = before[0], after[0]
for old, new in zip(a, b):
    assert old['scene'] == new['scene']
    for key in ('vertices', 'faces', 'payload_bytes', 'surface_area', 'surface_rms_m', 'surface_max_m',
                'face_normal_mean_deg', 'coverage_rms_m', 'coverage_p95_m', 'degenerate_faces',
                'nonmanifold_edges', 'winding_errors', 'boundary_edges', 'gap_bridge_faces',
                'triangle_quality_area_mean', 'skinny_area_fraction', 'zero_normals', 'nonunit_normals'):
        assert old[key] == new[key], (old['scene'], key, old[key], new[key])
    print(json.dumps(dict(scene=old['scene'], resolution=old['resolution'],
        angular_error=[old['shading_normal_mean_deg'], new['shading_normal_mean_deg']],
        seam_p95=[old['seam_normal_p95_deg'], new['seam_normal_p95_deg']],
        seam_max=[old['max_seam_normal_angle_deg'], new['max_seam_normal_angle_deg']],
        extraction_ms=[old['extract_ms'], new['extract_ms']], update_ms=[old['update_ms'], new['update_ms']])))
print(json.dumps(dict(summary=True, repeats=[len(before), len(after)],
    total_extract_ms=[statistics.median(sum(row['extract_ms'] for row in run) for run in results) for results in (before,after)],
    total_update_ms=[statistics.median(sum(row['update_ms'] for row in run) for run in results) for results in (before,after)])))
