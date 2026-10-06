package com.lvonasek.record;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.media.AudioManager;
import android.media.MediaActionSound;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.media.MediaRecorder;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Environment;
import android.os.SystemClock;
import android.provider.MediaStore;
import android.util.Log;

import com.lvonasek.gles.GLESSurfaceView;

import org.jcodec.api.SequenceEncoder;
import org.jcodec.common.Codec;
import org.jcodec.common.Format;
import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.model.Picture;
import org.jcodec.common.model.Rational;
import org.jcodec.scale.BitmapUtil;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import javax.microedition.khronos.opengles.GL10;

public class Recorder {

    //audio objects
    private static File mAudioFile;
    private static MediaRecorder mAudioEncoder;

    //video objects
    private static File mVideoFile;
    private static SequenceEncoder mVideoEncoder;
    private static long mVideoFrames;
    private static SeekableByteChannel mVideoOut;
    private static long mVideoTimestamp;

    //locked orientation
    private static int mOrientation;

    //video capturing
    private static CaptureBuffer mCaptureBuffer;
    private static final Object mLock = new Object();
    private static boolean mRecording;
    // One admitted frame owns the readback/conversion buffers until encoding ends.
    private static boolean mFramePending;
    private static ExecutorService mVideoWorker;

    //settings
    private static File mCustomRoot = null;
    private static int mVideoDownscale = 640;
    private static int mVideoFPS = 15;

    public static void capturePhoto(GL10 gl, GLESSurfaceView view) {
        int w = view.getWidth();
        int h = view.getHeight();
        Bitmap bitmap = createBitmapFromGLSurface(0, 0, w, h, gl, 1);
        if (bitmap != null) {
            Context context = view.getContext();
            playShooterSound(context, MediaActionSound.SHUTTER_CLICK);
            try {
                FileDescriptor file = context.getContentResolver().openFileDescriptor(getFile(context, false), "rw").getFileDescriptor();
                FileOutputStream out = new FileOutputStream(file);
                bitmap.compress(Bitmap.CompressFormat.JPEG, 90, out);
                out.flush();
                out.close();

                MediaScannerConnection.scanFile(context,
                        new String[] { file.toString() }, null,
                        (path, uri) -> {
                            Log.i("ExternalStorage", "Scanned " + path + ":");
                            Log.i("ExternalStorage", "-> uri=" + uri);
                        });
            }
            catch (Exception e) {
                e.printStackTrace();
            } finally {
                bitmap.recycle();
            }
        }
    }

    public static void captureVideoFrame(GL10 gl, GLESSurfaceView view, boolean addTimestamp, int frameSkip, boolean multithread) {
        final long count;
        final SequenceEncoder encoder;
        final ExecutorService worker;
        synchronized (mLock) {
            // Backpressure happens before GPU readback. Do not queue threads or
            // overwrite pixels that an encoder is still consuming.
            if (!mRecording || mVideoEncoder == null || mFramePending) return;
            if (frameSkip <= 0) {
                long elapsed = Math.max(0, SystemClock.elapsedRealtime() - mVideoTimestamp);
                // Equivalent to the old timestamp loop, including its first frame.
                long due = ((elapsed + 1) * mVideoFPS + 999) / 1000;
                count = Math.max(0, due - mVideoFrames);
                mVideoFrames += count;
            } else {
                count = mVideoFrames % frameSkip == 0 ? 1 : 0;
                mVideoFrames++;
            }
            if (count == 0) return;
            mFramePending = true;
            encoder = mVideoEncoder;
            worker = mVideoWorker;
        }
        boolean handedOff = false;
        try {
            int w = view.getWidth();
            int h = view.getHeight();
            int s = Math.max(w, h) / mVideoDownscale + 1;
            if (mCaptureBuffer == null || !mCaptureBuffer.matches(w, h, s)) {
                if (mCaptureBuffer != null) mCaptureBuffer.recycle();
                mCaptureBuffer = null;
                mCaptureBuffer = new CaptureBuffer(w, h, s);
            }
            final CaptureBuffer buffer = mCaptureBuffer;
            final long timestamp = System.currentTimeMillis();
            buffer.readBuffer.position(0);
            gl.glReadPixels(0, 0, w, h, GL10.GL_RGBA, GL10.GL_UNSIGNED_BYTE, buffer.readBuffer);
            Runnable task = () -> {
                try {
                    captureFrame(buffer, encoder, timestamp, count, addTimestamp);
                } finally {
                    finishFrame();
                }
            };
            if (multithread && frameSkip <= 0) {
                worker.execute(task);
                handedOff = true;
            } else {
                handedOff = true;
                task.run();
            }
        } catch (RuntimeException | Error e) {
            if (!handedOff) finishFrame();
            throw e;
        }
    }

    private static void finishFrame() {
        synchronized (mLock) {
            mFramePending = false;
            mLock.notifyAll();
        }
    }

    public static boolean isVideoRecording() {
        synchronized (mLock) {
            return mRecording && mVideoEncoder != null;
        }
    }

    public static void startCapturingVideo(Activity context, boolean recordAudio) {
        synchronized (mLock) {
            if (mRecording || mVideoEncoder != null) return;
            startCapturingVideoLocked(context, recordAudio);
        }
    }

    private static void startCapturingVideoLocked(Activity context, boolean recordAudio) {

        //lock screen orientation
        mOrientation = context.getRequestedOrientation();
        context.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LOCKED);
        playShooterSound(context, MediaActionSound.START_VIDEO_RECORDING);

        //prepare audio
        if (recordAudio) {
            mAudioFile = new File(getRootPath(), "audio_render.3gp");
            mAudioEncoder = new MediaRecorder();
            mAudioEncoder.setAudioSource(MediaRecorder.AudioSource.MIC);
            mAudioEncoder.setOutputFormat(MediaRecorder.OutputFormat.THREE_GPP);
            mAudioEncoder.setAudioEncoder(MediaRecorder.AudioEncoder.AMR_NB);
            mAudioEncoder.setAudioEncodingBitRate(128000);
            mAudioEncoder.setAudioSamplingRate(44100);
            mAudioEncoder.setOutputFile(mAudioFile.getAbsolutePath());
            try {
                mAudioEncoder.prepare();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }

        //prepare video
        try {
            Rational fps = Rational.R(mVideoFPS, 1);
            mVideoFile = new File(getRootPath(), "video_render.mp4");
            mVideoOut = NIOUtils.writableFileChannel(mVideoFile.getAbsolutePath());
            mVideoEncoder = new SequenceEncoder(mVideoOut, fps, Format.MOV, Codec.H264, null);
            mVideoFrames = 0;
            mVideoTimestamp = SystemClock.elapsedRealtime();
        } catch (Exception e) {
            e.printStackTrace();
        }

        //start
        if (recordAudio) {
            try {
                mAudioEncoder.start();
                mVideoTimestamp = SystemClock.elapsedRealtime();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
        mVideoWorker = Executors.newSingleThreadExecutor();
        mRecording = mVideoEncoder != null;
    }

    public static void stopCapturingVideo(Activity context, boolean saveIntoGallery) {

        //cancel recording
        synchronized (mLock) {
            mRecording = false;
        }

        //stop audio
        if (mAudioEncoder != null) {
            try {
                mAudioEncoder.stop();
            } catch (RuntimeException e) {
                e.printStackTrace();
            } finally {
                mAudioEncoder.release();
                mAudioEncoder = null;
            }
        }

        //stop video
        boolean interrupted = false;
        synchronized (mLock) {
            while (mFramePending) {
                try {
                    mLock.wait();
                } catch (InterruptedException e) {
                    // Finish the admitted frame before closing its encoder.
                    interrupted = true;
                }
            }
            try {
                if (mVideoEncoder != null) mVideoEncoder.finish();
            } catch (Exception e) {
                e.printStackTrace();
            } finally {
                try {
                    if (mVideoOut != null) mVideoOut.close();
                } catch (Exception e) {
                    e.printStackTrace();
                }
                mVideoEncoder = null;
                mVideoOut = null;
                if (mVideoWorker != null) mVideoWorker.shutdown();
                mVideoWorker = null;
                if (mCaptureBuffer != null) mCaptureBuffer.recycle();
                mCaptureBuffer = null;
            }
        }
        if (interrupted) Thread.currentThread().interrupt();

        //mix files
        if (saveIntoGallery) {
            mixTracks(context);
            mVideoFile.delete();
            if (mAudioFile != null) {
                mAudioFile.delete();
            }
            mAudioFile = null;
            mVideoFile = null;
        }

        playShooterSound(context, MediaActionSound.STOP_VIDEO_RECORDING);
        context.setRequestedOrientation(mOrientation);
    }

    public static File getVideoFile() {
        return mVideoFile;
    }

    public static int getVideoFPS() {
        return mVideoFPS;
    }

    public static void setCustomRoot(File file) {
        mCustomRoot = file;
    }

    public static void setVideoDownscale(int downscale) {
        mVideoDownscale = downscale;
    }

    public static void setVideoFPS(int fps) {
        mVideoFPS = fps;
    }

    private static Bitmap createBitmapFromGLSurface(int x, int y, int w, int h, GL10 gl, int s) {
        // A photo must not overwrite a video frame currently being encoded.
        CaptureBuffer buffer = new CaptureBuffer(w, h, s);
        gl.glReadPixels(x, y, w, h, GL10.GL_RGBA, GL10.GL_UNSIGNED_BYTE, buffer.readBuffer);
        return buffer.toBitmap();
    }

    private static class CaptureBuffer {
        final int width, height, scale, outputWidth, outputHeight;
        final int[] pixels, converted;
        final IntBuffer readBuffer;
        final Bitmap bitmap;
        private Picture picture;
        private Canvas canvas;
        private Paint paint;
        private SimpleDateFormat dateFormat, timeFormat;

        CaptureBuffer(int w, int h, int s) {
            width = w;
            height = h;
            scale = s;
            int ws = w / s, hs = h / s;
            if (ws % 2 == 1) ws--;
            if (hs % 2 == 1) hs--;
            outputWidth = ws;
            outputHeight = hs;
            converted = new int[ws * hs];
            pixels = new int[w * h];
            readBuffer = IntBuffer.wrap(pixels);
            bitmap = Bitmap.createBitmap(ws, hs, Bitmap.Config.ARGB_8888);
        }

        boolean matches(int w, int h, int s) {
            return width == w && height == h && scale == s;
        }

        Bitmap toBitmap() {
            for (int i = 0; i < outputHeight; i++) {
                int source = i * scale * width;
                int target = (outputHeight - i - 1) * outputWidth;
                for (int j = 0; j < outputWidth; j++) {
                    int pixel = pixels[source + j * scale];
                    converted[target + j] = (pixel & 0xff00ff00)
                            | (pixel << 16) & 0x00ff0000 | (pixel >> 16) & 0xff;
                }
            }
            bitmap.setPixels(converted, 0, outputWidth, 0, 0, outputWidth, outputHeight);
            return bitmap;
        }

        void drawTimestamp(long timestamp) {
            if (canvas == null) {
                canvas = new Canvas(bitmap);
                paint = new Paint();
                paint.setAntiAlias(true);
                paint.setColor(Color.WHITE);
                paint.setTextSize(16);
                dateFormat = new SimpleDateFormat("dd.MM.yyyy", Locale.US);
                timeFormat = new SimpleDateFormat("HH:mm", Locale.US);
            }
            Date date = new Date(timestamp);
            canvas.drawText(dateFormat.format(date), 16, 16, paint);
            canvas.drawText(timeFormat.format(date), 16, 16 + paint.getTextSize(), paint);
        }

        void recycle() {
            bitmap.recycle();
        }
    }

    private static Uri getFile(Context context, boolean video) {
        SimpleDateFormat formatter = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US);
        Date date = new Date(System.currentTimeMillis());
        String pkg = context.getPackageName();
        String type = video ? "Video_" : "Photo_";
        String extension = video ? ".mp4" : ".jpg";
        String name = type + formatter.format(date) + "_" + pkg + extension;

        ContentResolver resolver = context.getContentResolver();
        ContentValues contentValues = new ContentValues();
        contentValues.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        contentValues.put(MediaStore.MediaColumns.MIME_TYPE, video ? "video/mp4": "image/jpeg");
        contentValues.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DCIM);

        return resolver.insert(video ? MediaStore.Video.Media.EXTERNAL_CONTENT_URI : MediaStore.Images.Media.EXTERNAL_CONTENT_URI, contentValues);
    }

    private static void mixTracks(Context context) {
        try {
            FileDescriptor file = context.getContentResolver().openFileDescriptor(getFile(context, true), "rw").getFileDescriptor();

            MediaExtractor videoExtractor = new MediaExtractor();
            videoExtractor.setDataSource(mVideoFile.getAbsolutePath());

            MediaExtractor audioExtractor = new MediaExtractor();
            if (mAudioFile != null) {
                audioExtractor.setDataSource(mAudioFile.getAbsolutePath());
            }

            MediaMuxer muxer = new MediaMuxer(file, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);

            videoExtractor.selectTrack(0);
            MediaFormat videoFormat = videoExtractor.getTrackFormat(0);
            int videoTrack = muxer.addTrack(videoFormat);

            int audioTrack = 0;
            if (mAudioFile != null) {
                audioExtractor.selectTrack(0);
                MediaFormat audioFormat = audioExtractor.getTrackFormat(0);
                audioTrack = muxer.addTrack(audioFormat);
            }

            boolean sawEOS = false;
            int offset = 100;
            int sampleSize = 256 * 1024;
            ByteBuffer videoBuf = ByteBuffer.allocate(sampleSize);
            ByteBuffer audioBuf = ByteBuffer.allocate(sampleSize);
            MediaCodec.BufferInfo videoBufferInfo = new MediaCodec.BufferInfo();
            MediaCodec.BufferInfo audioBufferInfo = new MediaCodec.BufferInfo();


            videoExtractor.seekTo(0, MediaExtractor.SEEK_TO_CLOSEST_SYNC);
            if (mAudioFile != null) {
                audioExtractor.seekTo(0, MediaExtractor.SEEK_TO_CLOSEST_SYNC);
            }

            muxer.start();

            while (!sawEOS)
            {
                videoBufferInfo.offset = offset;
                videoBufferInfo.size = videoExtractor.readSampleData(videoBuf, offset);


                if (videoBufferInfo.size < 0 || audioBufferInfo.size < 0)
                {
                    sawEOS = true;
                    videoBufferInfo.size = 0;
                }
                else
                {
                    videoBufferInfo.presentationTimeUs = videoExtractor.getSampleTime();
                    videoBufferInfo.flags = videoExtractor.getSampleFlags();
                    muxer.writeSampleData(videoTrack, videoBuf, videoBufferInfo);
                    videoExtractor.advance();
                }
            }

            if (mAudioFile != null) {

                boolean sawEOS2 = false;
                while (!sawEOS2)
                {
                    audioBufferInfo.offset = offset;
                    audioBufferInfo.size = audioExtractor.readSampleData(audioBuf, offset);

                    if (videoBufferInfo.size < 0 || audioBufferInfo.size < 0)
                    {
                        sawEOS2 = true;
                        audioBufferInfo.size = 0;
                    }
                    else
                    {
                        audioBufferInfo.presentationTimeUs = audioExtractor.getSampleTime();
                        audioBufferInfo.flags = audioExtractor.getSampleFlags();
                        muxer.writeSampleData(audioTrack, audioBuf, audioBufferInfo);
                        audioExtractor.advance();
                    }
                }
            }

            muxer.stop();
            muxer.release();

            MediaScannerConnection.scanFile(context,
                    new String[] { file.toString() }, null,
                    (path, uri) -> {
                        Log.i("ExternalStorage", "Scanned " + path + ":");
                        Log.i("ExternalStorage", "-> uri=" + uri);
                    });

        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private static void playShooterSound(Context context, int sample) {
        AudioManager audio = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        switch( audio.getRingerMode() ){
            case AudioManager.RINGER_MODE_NORMAL:
                MediaActionSound sound = new MediaActionSound();
                sound.play(sample);
                break;
            case AudioManager.RINGER_MODE_SILENT:
                break;
            case AudioManager.RINGER_MODE_VIBRATE:
                break;
        }
    }

    private static void captureFrame(CaptureBuffer buffer, SequenceEncoder encoder, long timestamp,
                                     long count, boolean addTimestamp) {
        try {
            Bitmap bitmap = buffer.toBitmap();
            if (addTimestamp) buffer.drawTimestamp(timestamp);
            if (buffer.picture == null) {
                buffer.picture = BitmapUtil.fromBitmap(bitmap);
            } else {
                BitmapUtil.fromBitmap(bitmap, buffer.picture);
            }
            for (long i = 0; i < count; i++) {
                encoder.encodeNativeFrame(buffer.picture);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private static File getRootPath() {
        if (mCustomRoot != null) {
            return mCustomRoot;
        }
        return Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
    }
}
