#!/usr/bin/env python3
"""Compare fixed-recording replay results, including acceptance and resource costs."""
import argparse
import json
from pathlib import Path
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('before', type=Path)
parser.add_argument('after', type=Path)
parser.add_argument('--require-same-mesh', action='store_true',
                    help='For scheduling-only changes, require identical ordered output bytes')
args = parser.parse_args()
before, after = (json.loads(path.read_text()) for path in (args.before, args.after))
assert before['fixture_sha256'] == after['fixture_sha256'], 'Different input recordings'
assert before['input_state'] == after['input_state'], 'Different input calibration'
assert before['pose_conversion_body_sha256'] == after['pose_conversion_body_sha256'], 'Different pose conversion'
a, b = before['results'][0], after['results'][0]
assert a['config'] == b['config'], 'Different engine settings'
assert a['statuses'] == b['statuses'], 'Frame acceptance changed'
if args.require_same_mesh:
    assert a['ordered_mesh_sha256'] == b['ordered_mesh_sha256'], 'Output bytes changed'
    assert a['mesh'] == b['mesh'] and a['residuals'] == b['residuals'], 'Output diagnostics changed'
for key in ('enabled', 'resident_budget_bytes', 'backing_budget_bytes', 'max_logical_chunks'):
    assert a['paging'].get(key) == b['paging'].get(key), 'Different paging budgets'

summary = {'fixture_sha256': before['fixture_sha256'],
           'frames': len(a['statuses']),
           'accepted_frames': sum(status == 0 for status in a['statuses']),
           'scope': 'Host replay; real input residuals are self-consistency, not physical accuracy',
           'variants': {}}
for name, report in (('before', before), ('after', after)):
    rows = report['results']
    first = rows[0]
    # Canonical geometry and acceptance must agree across each variant's repeats.
    for repeat in rows[1:]:
        for key in ('config', 'statuses', 'mesh', 'residuals', 'ordered_mesh_sha256'):
            assert repeat[key] == first[key], 'Non-repeatable ' + key
    timing_key = next(key for key, value in first.items()
                      if isinstance(value, dict) and 'replay_wall_ms' in value)
    summary['variants'][name] = {
        'repeats': len(rows),
        'timing_medians_ms': {
            metric: statistics.median(
                sum(row[timing_key][metric]) if isinstance(row[timing_key][metric], list)
                else row[timing_key][metric] for row in rows)
            for metric in ('update_ms', 'extract_validate_replace_ms', 'replay_wall_ms')},
        'mesh': first['mesh'],
        'residuals': first['residuals']['all_eligible_sampled_inputs'],
        'paging': first['paging'],
        'memory': first['memory'],
        'ordered_mesh_sha256': first['ordered_mesh_sha256'],
    }
print(json.dumps(summary, indent=2, allow_nan=False))
