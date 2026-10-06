package com.lvonasek.gles;

import java.lang.ref.WeakReference;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGL11;
import javax.microedition.khronos.opengles.GL10;

// Compiles the production GL thread against a fake EGL boundary. This verifies
// shutdown ownership/notification, not driver behavior or Android rendering.
public class GLESThreadLifecycleTest {
    public static void main(String[] args) throws Exception {
        verifyShutdown(false, false);
        verifyShutdown(true, false);
        verifyShutdown(false, true);
        verifyStartupFailure();
        verifyContextLoss();
        System.out.println("PASS: shutdown/failure cleanup, partial startup failure, and context-loss recovery");
    }

    private static void verifyContextLoss() throws Exception {
        EGLHelper.destroyed.set(0);
        EGLHelper.finished.set(0);
        EGLHelper.contextLossPending = true;
        AtomicInteger contexts = new AtomicInteger();
        CountDownLatch recovered = new CountDownLatch(1);
        GLESSurfaceView view = new GLESSurfaceView();
        view.mRenderer = new GLESSurfaceView.Renderer() {
            public void onSurfaceCreated(GL10 gl, Object config) { contexts.incrementAndGet(); }
            public void onSurfaceChanged(GL10 gl, int width, int height) {}
            public void onDrawFrame(GL10 gl) {
                if (contexts.get() == 2) recovered.countDown();
            }
        };
        GLESThread thread = new GLESThread(new WeakReference<>(view));
        thread.surfaceCreated();
        thread.onWindowResize(16, 16);
        thread.start();
        if (!recovered.await(3, TimeUnit.SECONDS)) throw new AssertionError("Did not recreate the lost context");
        Thread waiter = new Thread(thread::requestExitAndWait);
        waiter.setDaemon(true);
        waiter.start();
        waiter.join(3000);
        thread.join(3000);
        if (waiter.isAlive() || thread.isAlive()) throw new AssertionError("Shutdown after recovery failed");
        if (contexts.get() != 2 || EGLHelper.destroyed.get() != 2 || EGLHelper.finished.get() != 2)
            throw new AssertionError("Lost or replacement context not released exactly once");
    }

    private static void verifyStartupFailure() throws Exception {
        EGLHelper.destroyed.set(0);
        EGLHelper.finished.set(0);
        EGLHelper.cleanupFails = false;
        EGLHelper.startFails = true;
        GLESSurfaceView view = new GLESSurfaceView();
        GLESThread thread = new GLESThread(new WeakReference<>(view));
        thread.surfaceCreated();
        thread.onWindowResize(16, 16);
        thread.start();
        thread.join(3000);
        if (thread.isAlive()) throw new AssertionError("Startup failure left the GL thread running");
        if (EGLHelper.finished.get() != 1 || EGLHelper.destroyed.get() != 0)
            throw new AssertionError("Partially initialized EGL helper was not released");
        EGLHelper.startFails = false;
    }

    private static void verifyShutdown(boolean rendererFails, boolean cleanupFails) throws Exception {
        EGLHelper.destroyed.set(0);
        EGLHelper.finished.set(0);
        EGLHelper.cleanupFails = cleanupFails;
        CountDownLatch drewFrame = new CountDownLatch(1);
        GLESSurfaceView view = new GLESSurfaceView();
        view.mRenderer = new GLESSurfaceView.Renderer() {
            public void onSurfaceCreated(GL10 gl, Object config) {}
            public void onSurfaceChanged(GL10 gl, int width, int height) {}
            public void onDrawFrame(GL10 gl) {
                drewFrame.countDown();
                if (rendererFails) throw new IllegalStateException("expected renderer failure");
            }
        };
        GLESThread thread = new GLESThread(new WeakReference<>(view));
        AtomicReference<Throwable> uncaught = new AtomicReference<>();
        thread.setUncaughtExceptionHandler((t, error) -> uncaught.set(error));
        thread.surfaceCreated();
        thread.onWindowResize(16, 16);
        thread.start();
        if (!drewFrame.await(3, TimeUnit.SECONDS)) throw new AssertionError("No frame rendered");

        Thread waiter = new Thread(thread::requestExitAndWait);
        waiter.setDaemon(true);
        waiter.start();
        waiter.join(3000);
        thread.join(3000);
        if (waiter.isAlive() || thread.isAlive()) throw new AssertionError("Shutdown did not notify waiters");
        if (EGLHelper.destroyed.get() != 1) throw new AssertionError("EGL surface cleanup count");
        if (EGLHelper.finished.get() != 1) throw new AssertionError("EGL context cleanup count");
        if (cleanupFails != (uncaught.get() != null)) throw new AssertionError("Unexpected cleanup exception state");
    }
}

class GLESSurfaceView {
    FrameTimings mFrameTimings;
    Renderer mRenderer;
    interface Renderer {
        void onSurfaceCreated(GL10 gl, Object config);
        void onSurfaceChanged(GL10 gl, int width, int height);
        void onDrawFrame(GL10 gl);
    }
}

class EGLHelper {
    static final AtomicInteger destroyed = new AtomicInteger();
    static final AtomicInteger finished = new AtomicInteger();
    static boolean cleanupFails;
    static boolean startFails;
    static boolean contextLossPending;
    EGLHelper(WeakReference<GLESSurfaceView> view) {}
    void start() {
        if (startFails) throw new IllegalStateException("expected EGL startup failure");
    }
    boolean createSurface() { return true; }
    Object createGL() { return (GL10) name -> "host-test"; }
    Object getConfig() { return null; }
    int swap() {
        if (contextLossPending) {
            contextLossPending = false;
            return EGL11.EGL_CONTEXT_LOST;
        }
        Thread.yield();
        return EGL10.EGL_SUCCESS;
    }
    void destroySurface() {
        destroyed.incrementAndGet();
        if (cleanupFails) throw new IllegalStateException("expected surface cleanup failure");
    }
    void finish() { finished.incrementAndGet(); }
}
