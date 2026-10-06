#!/usr/bin/env python3
"""Exercise the production compatibility bridge with a real JVM and old JNI ABI."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from java_tools import java_command, javac_command, jni_home

JDK = jni_home()
java=ROOT/'scanner/app/src/main/java/com/lvonasek/arcore3dscanner/main/JNI.java'
with tempfile.TemporaryDirectory(prefix='quality-jni-') as temp:
    work=Path(temp);(work/'android').mkdir()
    (work/'android/log.h').write_text('#pragma once\n#define ANDROID_LOG_INFO 4\ninline int __android_log_print(int,const char*,const char*,...){return 0;}\n')
    includes=['-I'+str(JDK/'include'),'-I'+str(JDK/'include/linux'),'-I'+str(work)]
    for name,source in [('3dscanner',ROOT/'tests/quality_native/fake_baseline.cc'),
                        ('scanner_quality_bridge',ROOT/'scanner/app/src/quality/jni/bridge.cc')]:
        subprocess.run(['g++','-std=c++11','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',*includes,str(source),'-ldl','-pthread','-o',str(work/('lib'+name+'.so'))],check=True,timeout=90)
    (work/'Context.java').write_text('package android.content; public class Context {}')
    (work/'Resources.java').write_text('package android.content.res; public class Resources { public String getString(int id){return "";} }')
    (work/'BuildConfig.java').write_text('package com.lvonasek.arcore3dscanner; public class BuildConfig { public static final String FLAVOR="quality"; }')
    names=sorted(set(re.findall(r'R\.string\.(\w+)',java.read_text())))
    (work/'R.java').write_text('package com.lvonasek.arcore3dscanner; public class R { public static class string {'+''.join('public static final int '+n+'='+str(i)+';' for i,n in enumerate(names))+'}}')
    (work/'Test.java').write_text(r'''
import java.nio.file.*;
import com.lvonasek.arcore3dscanner.main.JNI;
public class Test {
  public static void main(String[] args) throws Exception {
    Path root=Paths.get(args[0]);
    if(JNI.finishCapture())throw new AssertionError("no dataset");
    if(JNI.onGlSurfaceDrawFrame(false,12.5f,2,false,true,true))throw new AssertionError("background draw");
    JNI.onResume();
    if(!JNI.onARServiceConnected(null,.02,.01,4,9,false,false,false,true,false,1,true,
        root.toString().getBytes("UTF-8"),"ignored-cache".getBytes("UTF-8")))throw new AssertionError("connect");
    if(!JNI.onGlSurfaceDrawFrame(false,12.5f,2,false,true,true))throw new AssertionError("draw");
    JNI.onPause();
    if(JNI.onGlSurfaceDrawFrame(false,12.5f,2,false,true,true))throw new AssertionError("paused draw");
    if(!JNI.finishCapture()||!Files.isRegularFile(root.resolve("model.obj")))throw new AssertionError("save barrier");
    Path result=root.resolve("textured.obj");
    if(!JNI.texturize("input".getBytes(),result.toString().getBytes(),false,false))throw new AssertionError("texturing");
    byte[] saved=Files.readAllBytes(result);
    if(JNI.texturize("input".getBytes(),result.toString().getBytes(),false,false))throw new AssertionError("overwrite");
    if(!java.util.Arrays.equals(saved,Files.readAllBytes(result)))throw new AssertionError("file changed");
    if(JNI.texturize("fail".getBytes(),root.resolve("missing.obj").toString().getBytes(),false,false))throw new AssertionError("false success");
    if(JNI.getTexturingError().isEmpty())throw new AssertionError("missing error");
    JNI.setCoveragePreview(false);JNI.setExperimentalDepth(false);
    try {JNI.setExperimentalDepth(true);throw new AssertionError("unsafe option enabled");}
    catch(UnsupportedOperationException expected){}
    if(JNI.getScanSize()!=11112)throw new AssertionError("unexpected original calls: "+JNI.getScanSize());
    if(!JNI.getCaptureDiagnostics().contains("Original native"))throw new AssertionError("diagnostic");
    System.out.println("PASS real JVM: exact original ABI forwarding, lifecycle gate, original save barrier, fresh-output validation and unsupported-option rejection");
  }
}
''')
    subprocess.run(javac_command()+['-d',str(work),str(java),*[str(stub) for stub in work.glob('*.java')]],check=True,timeout=60)
    capture=work/'capture';capture.mkdir()
    subprocess.run(java_command()+['-Xcheck:jni','-Djava.library.path='+str(work),'-cp',str(work),'Test',str(capture)],check=True,timeout=30,
                   env=dict(os.environ,LD_LIBRARY_PATH=str(work)))
