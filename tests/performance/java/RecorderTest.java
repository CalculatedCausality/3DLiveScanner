// Actual Recorder compiled against real Android/JCodec APIs, then run with the
// labeled Android/surface/readback/encoder boundaries supplied by this suite.
import android.app.Activity;
import android.graphics.Bitmap;
import android.os.SystemClock;
import com.lvonasek.gles.GLESSurfaceView;
import com.lvonasek.record.Recorder;
import org.jcodec.api.SequenceEncoder;
import org.jcodec.common.model.Picture;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.nio.IntBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import javax.microedition.khronos.opengles.GL10;

public class RecorderTest {
    static int reads, frame;
    static boolean readFailure;
    static File outputDirectory;
    static final GLESSurfaceView view = new GLESSurfaceView();
    static final Activity activity = new Activity();
    static final GL10 gl = (GL10) Proxy.newProxyInstance(RecorderTest.class.getClassLoader(),
            new Class[]{GL10.class}, (proxy, method, args) -> {
                if (!method.getName().equals("glReadPixels")) throw new AssertionError("unexpected GL call: " + method);
                ++reads;
                if (readFailure) throw new IllegalStateException("injected readback failure");
                IntBuffer output = (IntBuffer) args[6];
                for (int i = 0; i < output.capacity(); i++) output.put(i, pixel(i));
                return null;
            });

    static int pixel(int i) {
        return 0xff000000 | ((i * 41 + frame) & 255) | (((i * 13) & 255) << 8) | (((i * 31) & 255) << 16);
    }
    static void check(boolean condition, String text) { if (!condition) throw new AssertionError(text); }

    static Object field(String name) throws Exception {
        Field field = Recorder.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(null);
    }

    static void awaitIdle() throws Exception {
        Object lock = field("mLock");
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        synchronized (lock) {
            while ((Boolean) field("mFramePending")) {
                check(System.nanoTime() < deadline, "idle timeout");
                lock.wait(20);
            }
        }
    }

    static void start() {
        Recorder.setCustomRoot(outputDirectory);
        Recorder.startCapturingVideo(activity, false);
        check(Recorder.isVideoRecording(), "start");
    }

    static void verifyPixels(byte[] output) {
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int p = pixel((7 - y) * 8 + x), i = (y * 8 + x) * 3;
            check(output[i] == (byte)((p & 255) - 128), "red or flip mismatch");
            check(output[i + 1] == (byte)(((p >>> 8) & 255) - 128), "green mismatch");
            check(output[i + 2] == (byte)(((p >>> 16) & 255) - 128), "blue mismatch");
        }
    }

    static void testReuse() throws Exception {
        start();
        SequenceEncoder encoder = SequenceEncoder.latest;
        Picture first = null;
        int baseline = Bitmap.allocations;
        for (frame = 0; frame < 100; frame++) {
            Recorder.captureVideoFrame(gl, view, false, 1, false);
            if (first == null) first = encoder.lastInput;
            check(first == encoder.lastInput, "Picture not reused");
            verifyPixels(encoder.lastPixels);
        }
        check(Bitmap.allocations - baseline == 1 && encoder.frames == 100, "bitmap allocation/frame count");
        Recorder.captureVideoFrame(gl, view, true, 1, false);
        check(encoder.lastPixels[0] == 127, "timestamp overlay");
        Recorder.captureVideoFrame(gl, view, false, 1, false);
        verifyPixels(encoder.lastPixels);
        Recorder.stopCapturingVideo(activity, false);
        check(encoder.finished && field("mCaptureBuffer") == null, "cleanup");
    }

    static void testOwnershipAndStop() throws Exception {
        start();
        SequenceEncoder encoder = SequenceEncoder.latest;
        encoder.entered = new CountDownLatch(1);
        encoder.release = new CountDownLatch(1);
        Recorder.captureVideoFrame(gl, view, false, 0, true);
        check(encoder.entered.await(2, TimeUnit.SECONDS), "worker not started");
        int readCount = reads;
        for (int i = 0; i < 1000; i++) {
            SystemClock.now++;
            frame++; // Any accidental readback/conversion would change owned pixels.
            Recorder.captureVideoFrame(gl, view, false, 0, true);
        }
        check(reads == readCount, "busy frame performed GPU readback");
        check((Long) field("mVideoFrames") == 1L, "busy frame consumed timeline slots");
        AtomicReference<Throwable> stopFailure = new AtomicReference<>();
        Thread stop = new Thread(() -> {
            try { Recorder.stopCapturingVideo(activity, false); }
            catch (Throwable failure) { stopFailure.set(failure); }
        });
        stop.start();
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (Recorder.isVideoRecording() && System.nanoTime() < deadline) Thread.yield();
        check(!Recorder.isVideoRecording(), "stop did not close admission");
        check(!encoder.finished, "encoder closed before worker completed");
        encoder.release.countDown();
        stop.join(2000);
        check(stopFailure.get() == null, "stop threw: " + stopFailure.get());
        check(!stop.isAlive() && encoder.finished && encoder.frames == 1, "stop did not drain admitted frame");
        check(field("mVideoWorker") == null && field("mCaptureBuffer") == null, "worker/buffer retained");
    }

    static void testFailuresAndFrameSkip() throws Exception {
        start();
        readFailure = true;
        try {
            Recorder.captureVideoFrame(gl, view, false, 1, false);
            throw new AssertionError("expected read failure");
        } catch (IllegalStateException expected) {}
        check(!(Boolean) field("mFramePending"), "read failure wedged capture");
        readFailure = false;
        SequenceEncoder.latest.fail = true;
        SystemClock.now += 100;
        Recorder.captureVideoFrame(gl, view, false, 0, true);
        awaitIdle();
        check(SequenceEncoder.latest.failures == 1, "failure injection was not exercised");
        SequenceEncoder.latest.fail = false;
        Recorder.captureVideoFrame(gl, view, false, 1, false);
        check(SequenceEncoder.latest.frames == 1, "encoder failure prevented recovery");
        Recorder.stopCapturingVideo(activity, false);
        start();
        for (int i = 0; i < 10; ++i) Recorder.captureVideoFrame(gl, view, false, 3, true);
        check(SequenceEncoder.latest.frames == 4, "frameSkip changed");
        Recorder.stopCapturingVideo(activity, false);
    }

    static void testScheduling() {
        for (int fps : new int[]{1, 15, 24, 30, 60}) {
            Recorder.setVideoFPS(fps);
            start();
            long oldCount = 0, beginning = SystemClock.now;
            for (long elapsed = 0; elapsed < 100000; elapsed++) {
                // Independent oracle: the original timestamp loop, not the new formula.
                while (oldCount * 1000 / fps <= elapsed) oldCount++;
                SystemClock.now = beginning + elapsed;
                Recorder.captureVideoFrame(gl, view, false, 0, false);
                check(SequenceEncoder.latest.frames == oldCount, "production scheduling diverged");
            }
            Recorder.stopCapturingVideo(activity, false);
        }
    }

    public static void main(String[] args) throws Exception {
        outputDirectory = new File(args[0]);
        try {
            testReuse();
            testOwnershipAndStop();
            testFailuresAndFrameSkip();
            testScheduling();
            System.out.println("PASS: Recorder ownership/backpressure, 100-frame Bitmap/Picture reuse, pixels, timestamp clearing, stop/restart, failure recovery, frameSkip and 500000 production scheduling samples (mock Android/encoder)");
        } finally {
            // Even a failed assertion must not leave the JVM's worker running.
            if (SequenceEncoder.latest != null && SequenceEncoder.latest.release != null)
                SequenceEncoder.latest.release.countDown();
            ExecutorService worker = (ExecutorService) field("mVideoWorker");
            if (worker != null) worker.shutdownNow();
        }
    }
}
