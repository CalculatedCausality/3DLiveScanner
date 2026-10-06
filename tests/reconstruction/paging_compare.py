#!/usr/bin/env python3
"""Compare recorded RAM/paged results and complete ordered mesh bytes."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("ram",type=Path)
parser.add_argument("paged",type=Path)
args = parser.parse_args()
a = json.loads((args.ram / "results.json").read_text())
b = json.loads((args.paged / "results.json").read_text())
assert a["fixture_sha256"] == b["fixture_sha256"]
x,y = a["results"][0],b["results"][0]
for field in ("config","statuses","points","dirty_segments","mesh","residuals","ordered_mesh_sha256"):
    assert x[field] == y[field], field
rejected = [i for i,status in enumerate(x["statuses"]) if status != 0]
assert not x["paging"]["enabled"] and y["paging"]["enabled"]
s = y["paging"]
assert s["logical_chunks"] == x["paging"]["logical_chunks"]
assert s["peak_resident_bytes"] <= s["resident_budget_bytes"]
assert s["backing_bytes"] <= s["backing_budget_bytes"]
assert not s["requires_replay"] and s["last_failure"] == 0
size = 0
digest = hashlib.sha256()
with (args.ram / "ordered_mesh_0.bin").open("rb") as left, (args.paged / "ordered_mesh_0.bin").open("rb") as right:
    while True:
        p,q = left.read(1024*1024),right.read(1024*1024)
        assert p == q, ("byte mismatch",size)
        if not p: break
        size += len(p); digest.update(p)
assert digest.hexdigest() == x["ordered_mesh_sha256"]
print(json.dumps(dict(equal_bytes=size,ordered_mesh_sha256=digest.hexdigest(),input_frames=len(x["statuses"]),
    accepted_frames=len(x["statuses"])-len(rejected),identically_rejected_frames=rejected,
    ram_diagnostics=x["diagnostics"],paged_diagnostics=y["diagnostics"],
    vertices=x["mesh"]["vertices"],faces=x["mesh"]["faces"],paging=s,
    ram_replay_ms=x["timing"]["replay_wall_ms"],paged_replay_ms=y["timing"]["replay_wall_ms"],
    ram_update_ms=sum(x["timing"]["update_ms"]),paged_update_ms=sum(y["timing"]["update_ms"]),
    ram_extract_ms=sum(x["timing"]["extract_validate_replace_ms"]),paged_extract_ms=sum(y["timing"]["extract_validate_replace_ms"]),
    ram_memory=x["memory"],paged_memory=y["memory"],
    read_payload_bytes=s["reads"]*s["chunk_bytes"],write_payload_bytes=s["writes"]*s["chunk_bytes"]),indent=2))
