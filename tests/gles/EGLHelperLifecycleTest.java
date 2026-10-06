package com.lvonasek.gles;

import java.lang.ref.WeakReference;
import javax.microedition.khronos.egl.*;

public class EGLHelperLifecycleTest {
    public static void main(String[] args) {
        FakeEgl api = new FakeEgl();
        EGLContext.api = api;
        GLESSurfaceView view = new GLESSurfaceView();
        EGLHelper first = new EGLHelper(new WeakReference<>(view));
        EGLHelper second = new EGLHelper(new WeakReference<>(view));
        first.start();
        second.start();
        if (api.created != 2) throw new AssertionError("Helpers share a cached EGL context");
        first.finish();
        first.finish();
        if (api.destroyed != 1 || api.terminated != 1) throw new AssertionError("Cleanup is not idempotent");
        second.finish();
        second.start();
        if (api.created != 3) throw new AssertionError("Restart reused a stale context");
        api.failDestroy = true;
        try {
            second.finish();
            throw new AssertionError("Expected injected driver failure");
        } catch (IllegalStateException expected) {
            if (api.terminated != 3) throw new AssertionError("Context failure skipped display cleanup");
        }
        second.finish();
        if (api.destroyed != 3 || api.terminated != 3) throw new AssertionError("Failed cleanup retried stale handles");
        System.out.println("PASS: independent EGL contexts, restart, idempotence, and failed cleanup");
    }
}

class GLESSurfaceView {
    Object getHolder() { return this; }
}

class EGLConfigChooser {
    EGLConfigChooser(int... sizes) {}
    EGLConfig chooseConfig(EGL10 egl, EGLDisplay display) { return new EGLConfig(); }
}

class FakeEgl implements EGL10 {
    int created, destroyed, terminated;
    boolean failDestroy;
    public EGLDisplay eglGetDisplay(Object display) { return new EGLDisplay(); }
    public boolean eglInitialize(EGLDisplay display, int[] version) { return true; }
    public EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, int[] attributes) {
        created++;
        return new EGLContext();
    }
    public boolean eglDestroyContext(EGLDisplay d, EGLContext c) {
        destroyed++;
        if (failDestroy) throw new IllegalStateException("expected driver failure");
        return true;
    }
    public boolean eglTerminate(EGLDisplay display) { terminated++; return true; }
    public EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, Object window, int[] attributes) {
        return new EGLSurface();
    }
    public boolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c) { return true; }
    public boolean eglDestroySurface(EGLDisplay d, EGLSurface surface) { return true; }
    public boolean eglSwapBuffers(EGLDisplay d, EGLSurface surface) { return true; }
    public int eglGetError() { return EGL_SUCCESS; }
}
