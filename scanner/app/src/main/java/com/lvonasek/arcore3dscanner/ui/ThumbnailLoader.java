package com.lvonasek.arcore3dscanner.ui;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import android.widget.ImageView;

import com.lvonasek.arcore3dscanner.main.Exporter;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** Main-thread bindings/cache; workers own only immutable paths and unpublished bitmaps. */
final class ThumbnailLoader {
  interface Decoder {
    Bitmap decode(File scan);
  }

  private static final class Result {
    final Bitmap bitmap;
    Result(Bitmap bitmap) { this.bitmap = bitmap; }
  }

  private static final class Binding {
    final String path;
    boolean complete;
    Binding(String path) { this.path = path; }
  }

  private final Handler main = new Handler(Looper.getMainLooper());
  private final WeakHashMap<ImageView, Binding> bindings = new WeakHashMap<>();
  private final Map<String, Job> jobs = new HashMap<>();
  private final LruCache<String, Result> cache;
  private final ThreadPoolExecutor workers;
  private final Decoder decoder;
  private final int maxJobs;
  private volatile int generation;
  private volatile boolean closed;

  ThumbnailLoader() {
    this(ThumbnailLoader::decode, 2, 32, 8 * 1024 * 1024);
  }

  // Package-private injection keeps concurrency tests independent of Android's image codecs.
  ThumbnailLoader(Decoder decoder, int threads, int queueSize, int cacheBytes) {
    this.decoder = decoder;
    maxJobs = threads + queueSize;
    cache = new LruCache<String, Result>(cacheBytes) {
      @Override protected int sizeOf(String path, Result result) {
        // Charge misses too, so broken/missing thumbnails cannot grow an unbounded negative cache.
        return result.bitmap == null ? 1024 : Math.max(1024, result.bitmap.getAllocationByteCount());
      }
    };
    workers = new ThreadPoolExecutor(threads, threads, 10, TimeUnit.SECONDS,
            new ArrayBlockingQueue<>(queueSize), runnable -> new Thread(runnable, "scan-thumbnail"));
    workers.allowCoreThreadTimeOut(true);
  }

  void bind(ImageView view, File scan) {
    bindings.remove(view);
    if (closed) return;
    if (scan == null) {
      pump();
      return;
    }
    String path = scan.getAbsolutePath();
    Binding binding = new Binding(path);
    bindings.put(view, binding);
    Result result = cache.get(path);
    if (result != null) {
      binding.complete = true;
      if (result.bitmap != null) view.setImageBitmap(result.bitmap);
    }
    pump();
  }

  /** Invalidates running results as well as queued requests before a folder/contents change. */
  void clear() {
    ++generation;
    bindings.clear();
    workers.getQueue().clear();
    jobs.clear();
    // Displayed bitmaps can outlive the cache. Do not recycle published bitmaps on eviction.
    cache.evictAll();
  }

  void close() {
    closed = true;
    clear();
    workers.shutdownNow();
  }

  private void pump() {
    if (closed) return;
    LinkedHashSet<String> pending = new LinkedHashSet<>();
    for (Binding binding : bindings.values()) {
      if (!binding.complete) pending.add(binding.path);
    }
    // Remove work for recycled rows before spending queue capacity on their replacements.
    jobs.values().removeIf(job -> !pending.contains(job.path) && workers.remove(job));
    for (String path : pending) {
      if (jobs.size() >= maxJobs) break;
      if (jobs.containsKey(path)) continue;
      Job job = new Job(path, generation);
      jobs.put(job.path, job);
      try {
        workers.execute(job);
      } catch (RejectedExecutionException full) {
        jobs.remove(job.path);
        // Older-generation workers may still be finishing. Their completion pumps again.
        break;
      }
    }
  }

  private final class Job implements Runnable {
    final String path;
    final int epoch;
    Job(String path, int epoch) { this.path = path; this.epoch = epoch; }

    @Override public void run() {
      Bitmap bitmap = null;
      try {
        if (!closed && epoch == generation) bitmap = decoder.decode(new File(path));
      } catch (RuntimeException | OutOfMemoryError ignored) {
        // Decode/allocation failures finish normally, releasing the job slot and retaining
        // the placeholder until refresh. Other Errors are deliberately not swallowed.
      }
      final Bitmap result = bitmap;
      if (!main.post(() -> finish(this, result))) recycle(result);
    }
  }

  private void finish(Job job, Bitmap bitmap) {
    if (closed || job.epoch != generation) {
      recycle(bitmap);
      pump();
      return;
    }
    jobs.remove(job.path);
    cache.put(job.path, new Result(bitmap));
    for (Map.Entry<ImageView, Binding> entry : bindings.entrySet()) {
      ImageView view = entry.getKey();
      Binding binding = entry.getValue();
      if (view != null && binding.path.equals(job.path)) {
        binding.complete = true;
        if (bitmap != null) view.setImageBitmap(bitmap);
      }
    }
    // Requests beyond capacity stay weakly bound and get a turn as jobs finish, never dropped.
    pump();
  }

  static Bitmap decode(File scan) {
    if (!scan.isDirectory()) return null;
    File thumbnail = new File(scan, "thumbnail.jpg");
    Bitmap bitmap = sampledDecode(thumbnail);
    if (bitmap == null) {
      File source = null;
      boolean dataset = scan.getName().endsWith(Exporter.EXT_DATASET);
      if (dataset) {
        source = new File(scan, "00000000.jpg");
      } else if (scan.getName().endsWith(Exporter.EXT_OBJ)) {
        File model = AbstractActivity.getModel(scan);
        if (model != null && model.isFile()) {
          String mtl = Exporter.getMtlResource(model.getAbsolutePath());
          if (mtl != null) source = new File(model.getParentFile(), mtl + ".png");
        }
      }
      bitmap = sampledDecode(source);
      if (bitmap == null) return null;
      try {
        Bitmap scaled = Bitmap.createScaledBitmap(bitmap, dataset ? 180 : 256,
                dataset ? 320 : 256, false);
        if (scaled != bitmap) recycle(bitmap);
        bitmap = scaled;
        saveThumbnail(thumbnail, bitmap);
      } catch (RuntimeException | OutOfMemoryError failure) {
        recycle(bitmap);
        return null;
      }
    }
    try {
      int size = Math.min(bitmap.getWidth(), bitmap.getHeight());
      Bitmap cropped = Bitmap.createBitmap(bitmap, (bitmap.getWidth() - size) / 2,
              (bitmap.getHeight() - size) / 2, size, size);
      if (cropped != bitmap) recycle(bitmap);
      bitmap = cropped;
      Bitmap scaled = Bitmap.createScaledBitmap(bitmap, 256, 256, true);
      if (scaled != bitmap) recycle(bitmap);
      bitmap = null; // Ownership transfers to the cache/UI, including when scaling is a no-op.
      return scaled;
    } finally {
      recycle(bitmap);
    }
  }

  private static Bitmap sampledDecode(File file) {
    if (file == null || !file.isFile() || Thread.currentThread().isInterrupted()) return null;
    BitmapFactory.Options options = new BitmapFactory.Options();
    options.inJustDecodeBounds = true;
    BitmapFactory.decodeFile(file.getAbsolutePath(), options);
    if (options.outWidth <= 0 || options.outHeight <= 0) return null;
    // Bound BOTH dimensions, including panoramic/corrupt imported images.
    options.inSampleSize = 1;
    while (options.outWidth > 512L * options.inSampleSize
            || options.outHeight > 512L * options.inSampleSize) options.inSampleSize *= 2;
    options.inJustDecodeBounds = false;
    return BitmapFactory.decodeFile(file.getAbsolutePath(), options);
  }

  private static void saveThumbnail(File thumbnail, Bitmap bitmap) {
    // Best-effort preservation of existing cache entries, not an atomic no-clobber guarantee.
    if (thumbnail.exists() || Thread.currentThread().isInterrupted()) return;
    File temporary = null;
    try {
      temporary = File.createTempFile(".thumbnail-", ".tmp", thumbnail.getParentFile());
      try (FileOutputStream out = new FileOutputStream(temporary)) {
        if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 75, out)) return;
        out.flush();
        out.getFD().sync();
      }
      if (!Thread.currentThread().isInterrupted() && !thumbnail.exists() && temporary.length() > 0) {
        // Same-directory rename publishes only a completely encoded, closed JPEG.
        // A concurrent writer's cache entry can still be replaced after the existence check.
        temporary.renameTo(thumbnail);
      }
    } catch (IOException | SecurityException ignored) {
      // Read-only/full storage still gets the in-memory thumbnail.
    } finally {
      if (temporary != null) temporary.delete(); // Only our own temporary file, never scan data.
    }
  }

  private static void recycle(Bitmap bitmap) {
    if (bitmap != null) bitmap.recycle();
  }
}
