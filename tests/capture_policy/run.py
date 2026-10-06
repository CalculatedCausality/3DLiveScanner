#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tests'))
from java_tools import java_command, javac_command
from control_save.run import definition
with tempfile.TemporaryDirectory(prefix='capture-policy-',dir='/tmp/opencode') as directory:
    work=Path(directory)
    text=(ROOT/'tests/capture_policy/test.cc.in').read_text().replace('// FINISH_CAPTURE',definition('common/thread/reconstr.cc','bool Reconstruction::FinishCapture()')).replace('// PREPARE_FULL',definition('common/thread/reconstr.cc','bool Reconstruction::PrepareFullResolution()'))
    (work/'test.cc').write_text(text)
    subprocess.run(['g++','-std=c++11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(ROOT/'common'),str(work/'test.cc'),'-o',str(work/'test')],check=True,timeout=90)
    subprocess.run([str(work/'test'),str(work)],check=True,timeout=30,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    source_java=(ROOT/'tests/capture_policy/RawSave.java.in').read_text().replace('// SAVE_BODY',definition('scanner/app/src/main/java/com/lvonasek/arcore3dscanner/main/Main.java','  private void save()'))
    (work/'Main.java').write_text(source_java)
    subprocess.run(javac_command()+['-d',str(work),str(work/'Main.java')],check=True,timeout=60)
    subprocess.run(java_command()+['-ea','-cp',str(work),'Main',str(work)],check=True,timeout=30)
