#!/usr/bin/env python3
"""Exercise acquisition guards with a strict fake ADB; never contacts a device."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

sys.dont_write_bytecode = True
import recorded

FAKE = r'''#!__PYTHON__
import hashlib, json, shlex, shutil, sys
from pathlib import Path
root = Path(__ROOT__)
source = Path(__SOURCE__)
mode = (root / "mode").read_text()
logfile = root / "calls.json"
log = json.loads(logfile.read_text()) if logfile.exists() else {"states":0,"pulls":[]}
args = sys.argv[1:]
assert args[:2] == ["-s","fake-serial"]
action = args[2]
remote = "/authorized dataset"
allowed = ["state.txt"] + [f"{i:08d}{ext}" for i in range(6) for ext in (".pcl",".mat")]
if action == "shell":
    command = args[3]
    if command.startswith("run-as "):
        wrapped = shlex.split(command)
        assert wrapped[:4] == ["run-as","com.lvonasek.arcore3dscanner","sh","-c"]
        command = wrapped[4]
    if command.startswith("cat "):
        assert shlex.split(command) == ["cat",remote+"/state.txt"]
        log["states"] += 1
        logfile.write_text(json.dumps(log))
        if mode == "missing-state": sys.exit(1)
        data = (source/"state.txt").read_text()
        if mode == "changing" and log["states"] > 1: data = data.replace("6 48", "7 48", 1)
        print(data,end="")
    elif command.startswith("for f in "):
        for name in allowed[1:]:
            if mode == "incomplete" and name == "00000001.mat": continue
            print(remote+"/"+name+"|"+str((source/name).stat().st_size)+"|1")
        if mode == "stable-extra":
            print(remote+"/00000006.pcl|4|1")
            print(remote+"/00000006.mat|200|1")
    elif command.startswith("sha256sum "):
        for path in shlex.split(command)[1:]:
            name = path.rsplit("/",1)[-1]
            assert name in allowed and path == remote+"/"+name
            digest = hashlib.sha256((source/name).read_bytes()).hexdigest()
            if mode == "mutated-during-pull" and log["pulls"] and name == "state.txt": digest = "0"*64
            print(digest+"  "+path)
    else: raise AssertionError("Forbidden/unknown remote command: "+command)
elif action == "pull":
    path,destination = args[3:]
    name = path.rsplit("/",1)[-1]
    assert name in allowed and path == remote+"/"+name
    log["pulls"].append(name)
    logfile.write_text(json.dumps(log))
    shutil.copyfile(source/name,destination)
elif action == "exec-out":
    wrapped = shlex.split(args[3])
    assert wrapped[:4] == ["run-as","com.lvonasek.arcore3dscanner","sh","-c"]
    command = shlex.split(wrapped[4])
    assert command[0] == "cat"
    path = command[1]
    name = path.rsplit("/",1)[-1]
    assert name in allowed and path == remote+"/"+name
    log["pulls"].append(name)
    logfile.write_text(json.dumps(log))
    sys.stdout.buffer.write((source/name).read_bytes())
else: raise AssertionError("Forbidden ADB action: "+action)
'''


with tempfile.TemporaryDirectory(prefix="recorded-acquisition-",dir="/tmp/opencode") as temporary:
    root = Path(temporary)
    source = root / "source"
    recorded.synthetic(source)
    fake = root / "adb"
    fake.write_text(FAKE.replace("__PYTHON__",sys.executable).replace("__ROOT__",repr(str(root)))
                   .replace("__SOURCE__",repr(str(source))))
    fake.chmod(0o700)
    for variant in ("public", "private"):
      for mode in ("missing-state","changing","incomplete","mutated-during-pull","stable-extra"):
        (root / "mode").write_text(mode)
        (root / "calls.json").write_text(json.dumps(dict(states=0,pulls=[])))
        output = root / (variant + "-" + mode)
        args = SimpleNamespace(adb=fake,serial="fake-serial",remote="/authorized dataset",output=output,
                               expect_count=6,expect_width=48,expect_height=48,
                               run_as="com.lvonasek.arcore3dscanner" if variant == "private" else None)
        if mode == "stable-extra":
            with contextlib.redirect_stdout(io.StringIO()):
                recorded.acquire(args)
            manifest = recorded.validate(output)
            assert manifest["ignored_uncommitted_frame_files"] == 2
            assert manifest["raw_file_count"] == 13
            assert manifest["state_before_sha256"] == manifest["state_after_sha256"]
        else:
            try: recorded.acquire(args)
            except (ValueError,subprocess.SubprocessError): pass
            else: raise AssertionError("Acquisition should reject " + mode)
            assert not (output / "manifest.json").exists()
        calls = json.loads((root / "calls.json").read_text())
        if mode in ("missing-state","changing","incomplete"):
            assert not calls["pulls"]
        else:
            assert set(calls["pulls"]) == set(recorded.names(6))
    print("Fake-ADB public/private acquisition tests passed: missing/changing/incomplete/mid-pull mutation and stable committed prefix; no device accessed.")
