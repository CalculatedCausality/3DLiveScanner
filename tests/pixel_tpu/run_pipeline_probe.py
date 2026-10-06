#!/usr/bin/env python3
"""Read-only app state; generated standalone TPU benchmarks in unique scratch."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import uuid

ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--sdk',type=Path,required=True)
parser.add_argument('--serial',required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--cpu',type=int)
args=parser.parse_args()
out=args.output.resolve()
assert Path('/tmp/opencode') in out.parents
assert args.cpu is None or 0<=args.cpu<32
out.mkdir(exist_ok=False)
sources=[ROOT/'tests/pixel_tpu/pipeline_probe.cc',ROOT/'common/depth/experimental.cc',
         ROOT/'common/depth/experimental.h',ROOT/'common/depth/model_data.h',Path(__file__).resolve()]
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
hashes={str(p):digest(p) for p in sources}
compiler=args.sdk/'ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++'
command=[str(compiler),'-std=c++11','-O2','-Wall','-Wextra','-Werror','-fno-fast-math',
         '-I'+str(ROOT/'third_party/glm'),str(sources[0]),'-static-libstdc++','-ldl','-llog',
         '-Wl,-z,max-page-size=16384','-Wl,-z,common-page-size=16384','-o',str(out/'probe')]
subprocess.run(command,check=True,timeout=180)
adb=[str(args.sdk/'platform-tools/adb'),'-s',args.serial]
remote='/data/local/tmp/tpu-pipeline-'+uuid.uuid4().hex
subprocess.run(adb+['shell','ls','-d','/data/local/tmp'],check=True,timeout=15)
subprocess.run(adb+['shell','mkdir',remote],check=True,timeout=15)
result=None
try:
    subprocess.run(adb+['push',str(out/'probe'),remote+'/probe'],check=True,timeout=30)
    subprocess.run(adb+['shell','chmod','700',remote+'/probe'],check=True,timeout=15)
    invoke=adb+['shell','nice','-n','10']
    if args.cpu is not None:invoke+=['taskset',format(1<<args.cpu,'x')]
    invoke+=[remote+'/probe']
    result=subprocess.run(invoke,capture_output=True,text=True,timeout=150)
    (out/'stdout.txt').write_text(result.stdout);(out/'stderr.txt').write_text(result.stderr)
    print(result.stdout,flush=True)
    report={'source_sha256':hashes,'compile_command':command,'command':invoke,'exit_code':result.returncode,
            'rows':[json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')],
            'app_camera_dataset_settings_modified':False,'pinned_cpu':args.cpu,
            'scope':'Synthetic pipeline/eligibility offload latency; not full scan FPS or model accuracy'}
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    if result.returncode:print(result.stderr,flush=True)
    result.check_returncode()
    assert len(report['rows'])==4
finally:
    subprocess.run(adb+['shell','rm','-rf',remote],check=True,timeout=20)
    assert all(digest(Path(p))==h for p,h in hashes.items()),'Probe sources changed during execution'
