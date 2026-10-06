// Real JCodec encoder + BitmapUtil + MP4 demuxer; only Android Bitmap is mocked.
import android.graphics.Bitmap;
import org.jcodec.api.SequenceEncoder;
import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.model.Packet;
import org.jcodec.common.model.Picture;
import org.jcodec.common.model.Rational;
import org.jcodec.common.DemuxerTrack;
import org.jcodec.containers.mp4.demuxer.MP4Demuxer;
import org.jcodec.scale.BitmapUtil;
import java.io.File;

public class CodecReuseTest {
    static File encode(File directory, boolean reuse) throws Exception {
        File file = new File(directory, reuse ? "reuse.mp4" : "baseline.mp4");
        try (SeekableByteChannel out = NIOUtils.writableChannel(file)) {
            SequenceEncoder encoder = SequenceEncoder.createWithFps(out, Rational.R(15, 1));
            Bitmap bitmap = Bitmap.createBitmap(64, 48, Bitmap.Config.ARGB_8888);
            Picture picture = null;
            int[] pixels = new int[64 * 48];
            for (int frame = 0; frame < 30; frame++) {
                for (int i = 0; i < pixels.length; i++)
                    pixels[i] = 0xff000000 | ((i * 5171 + frame * 37291) & 0xffffff);
                bitmap.setPixels(pixels, 0, 64, 0, 0, 64, 48);
                if (!reuse || picture == null) picture = BitmapUtil.fromBitmap(bitmap);
                else BitmapUtil.fromBitmap(bitmap, picture);
                encoder.encodeNativeFrame(picture);
            }
            encoder.finish();
            bitmap.recycle();
        }
        return file;
    }

    public static void main(String[] args) throws Exception {
        File baseline = encode(new File(args[0]), false), reuse = encode(new File(args[0]), true);
        try (SeekableByteChannel a = NIOUtils.readableChannel(baseline);
             SeekableByteChannel b = NIOUtils.readableChannel(reuse)) {
            DemuxerTrack at = MP4Demuxer.createMP4Demuxer(a).getVideoTrack();
            DemuxerTrack bt = MP4Demuxer.createMP4Demuxer(b).getVideoTrack();
            int count = 0;
            Packet x, y;
            while ((x = at.nextFrame()) != null) {
                y = bt.nextFrame();
                if (y == null || !x.getData().equals(y.getData()) || x.getPts() != y.getPts()
                        || x.getDuration() != y.getDuration()) throw new AssertionError("packet mismatch at " + count);
                count++;
            }
            if (bt.nextFrame() != null || count != 30) throw new AssertionError("frame count");
            System.out.println("PASS: real JCodec produces identical H264 packets/PTS/durations for 30 fresh-vs-reused Picture frames");
        }
    }
}
