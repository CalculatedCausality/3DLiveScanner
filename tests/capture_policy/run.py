#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'common/thread/reconstr.cc').read_text()
def definition(signature):
    start=source.index(signature);opening=source.index('{',start);depth=0
    for end in range(opening,len(source)):
        depth+=(source[end]=='{')-(source[end]=='}')
        if depth==0:return source[start:end+1]
    raise AssertionError(signature)
with tempfile.TemporaryDirectory(prefix='capture-policy-',dir='/tmp/opencode') as directory:
    work=Path(directory)
    text=(ROOT/'tests/capture_policy/test.cc.in').read_text().replace('// FINISH_CAPTURE',definition('bool Reconstruction::FinishCapture()')).replace('// PREPARE_FULL',definition('bool Reconstruction::PrepareFullResolution()'))
    (work/'test.cc').write_text(text)
    subprocess.run(['g++','-std=c++11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(ROOT/'common'),str(work/'test.cc'),'-o',str(work/'test')],check=True,timeout=90)
    subprocess.run([str(work/'test'),str(work)],check=True,timeout=30,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    java=(ROOT/'scanner/app/src/main/java/com/lvonasek/arcore3dscanner/main/Main.java').read_text()
    begin=java.index('  private void save()');opening=java.index('{',begin);depth=0;end=opening
    for end in range(opening,len(java)):
        depth+=(java[end]=='{')-(java[end]=='}')
        if depth==0:break
    source_java=(ROOT/'tests/capture_policy/RawSave.java.in').read_text().replace('// SAVE_BODY',java[begin:end+1])
    (work/'Main.java').write_text(source_java)
    jdk=Path(os.environ.get('JAVA_HOME','/tmp/opencode/scanner-jdk17'))/'bin'
    subprocess.run([str(jdk/'javac'),'-d',str(work),str(work/'Main.java')],check=True,timeout=60)
    subprocess.run([str(jdk/'java'),'-ea','-cp',str(work),'Main',str(work)],check=True,timeout=30)
