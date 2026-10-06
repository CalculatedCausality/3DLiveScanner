#!/usr/bin/env python3
"""Run production GLESThread lifecycle checks with a fake EGL boundary and JDK 8+."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from java_tools import java_command, javac_command

JAVA = java_command()
JAVAC = javac_command()


with tempfile.TemporaryDirectory(prefix="scanner-gles-") as directory:
    work = Path(directory)
    stubs = {
        "javax/microedition/khronos/egl/EGL10.java":
            "package javax.microedition.khronos.egl; public interface EGL10 { int EGL_SUCCESS = 0x3000; }",
        "javax/microedition/khronos/egl/EGL11.java":
            "package javax.microedition.khronos.egl; public interface EGL11 { int EGL_CONTEXT_LOST = 0x300E; }",
        "javax/microedition/khronos/opengles/GL10.java":
            "package javax.microedition.khronos.opengles; public interface GL10 { int GL_RENDERER = 0x1F01; String glGetString(int name); }",
    }
    for name, content in stubs.items():
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    production = ROOT / "common/utils/com/lvonasek/gles"
    sources = [str(work / name) for name in stubs] + [
        str(production / "GLESThread.java"),
        str(production / "FrameTimings.java"),
        str(production / "GLESThreadManager.java"),
        str(ROOT / "tests/gles/GLESThreadLifecycleTest.java"),
    ]
    subprocess.run(JAVAC + ["-source", "8", "-target", "8", "-d", str(work)] + sources,
                   check=True, timeout=30)
    subprocess.run(JAVA + ["-cp", str(work), "com.lvonasek.gles.GLESThreadLifecycleTest"],
                   check=True, timeout=15)

# Compile the real EGL helper separately, replacing only its Android/EGL boundary.
with tempfile.TemporaryDirectory(prefix="scanner-egl-") as directory:
    work = Path(directory)
    stubs = {
        "android/util/Log.java":
            "package android.util; public class Log { public static int e(String tag, String message) { return 0; } }",
        "javax/microedition/khronos/opengles/GL.java":
            "package javax.microedition.khronos.opengles; public interface GL {}",
        "javax/microedition/khronos/egl/EGLContext.java":
            "package javax.microedition.khronos.egl; public class EGLContext { public static EGL10 api; public static Object getEGL() { return api; } public javax.microedition.khronos.opengles.GL getGL() { return null; } }",
        "javax/microedition/khronos/egl/EGL10.java": """
            package javax.microedition.khronos.egl;
            public interface EGL10 {
                int EGL_NONE = 0x3038, EGL_SUCCESS = 0x3000, EGL_BAD_NATIVE_WINDOW = 0x300B;
                Object EGL_DEFAULT_DISPLAY = new Object();
                EGLDisplay EGL_NO_DISPLAY = new EGLDisplay();
                EGLContext EGL_NO_CONTEXT = new EGLContext();
                EGLSurface EGL_NO_SURFACE = new EGLSurface();
                EGLDisplay eglGetDisplay(Object display);
                boolean eglInitialize(EGLDisplay display, int[] version);
                EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, int[] attributes);
                boolean eglDestroyContext(EGLDisplay d, EGLContext c);
                boolean eglTerminate(EGLDisplay display);
                EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, Object window, int[] attributes);
                boolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c);
                boolean eglDestroySurface(EGLDisplay d, EGLSurface surface);
                boolean eglSwapBuffers(EGLDisplay d, EGLSurface surface);
                int eglGetError();
            }
        """,
    }
    for name in ("EGLDisplay", "EGLSurface", "EGLConfig"):
        stubs["javax/microedition/khronos/egl/" + name + ".java"] = (
            "package javax.microedition.khronos.egl; public class " + name + " {}")
    for name, content in stubs.items():
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    sources = [str(work / name) for name in stubs] + [
        str(ROOT / "common/utils/com/lvonasek/gles/EGLHelper.java"),
        str(ROOT / "tests/gles/EGLHelperLifecycleTest.java"),
    ]
    subprocess.run(JAVAC + ["-source", "8", "-target", "8", "-d", str(work)] + sources,
                   check=True, timeout=30)
    subprocess.run(JAVA + ["-cp", str(work), "com.lvonasek.gles.EGLHelperLifecycleTest"],
                   check=True, timeout=15)
