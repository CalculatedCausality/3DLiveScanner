package org.jcodec.api;

import java.io.IOException;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.jcodec.common.Codec;
import org.jcodec.common.Format;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.model.Picture;
import org.jcodec.common.model.Rational;

// Only RecorderTest uses this controllable encoder. BitmapUtil/Picture remain real.
// CodecReuseTest has a separate classpath that cannot load this class.
public class SequenceEncoder {
    public static volatile SequenceEncoder latest;
    public volatile CountDownLatch entered, release;
    public volatile boolean finished, fail;
    public volatile int frames, failures;
    public Picture lastInput;
    public byte[] lastPixels;

    public SequenceEncoder(SeekableByteChannel out, Rational fps, Format f, Codec v, Codec a) { latest = this; }

    public void encodeNativeFrame(Picture picture) throws IOException {
        if (finished) throw new AssertionError("encode after finish");
        byte[] snapshot = picture.getPlaneData(0).clone();
        if (entered != null) {
            entered.countDown();
            try {
                if (!release.await(5, TimeUnit.SECONDS)) throw new AssertionError("encoder test timeout");
            } catch (InterruptedException e) {
                throw new AssertionError(e);
            }
            if (!Arrays.equals(snapshot, picture.getPlaneData(0)))
                throw new AssertionError("producer overwrote encoder-owned pixels");
        }
        if (fail) {
            ++failures;
            throw new IOException("EXPECTED: injected encoder failure");
        }
        lastInput = picture;
        lastPixels = snapshot;
        ++frames;
    }

    public void finish() { finished = true; }
}
