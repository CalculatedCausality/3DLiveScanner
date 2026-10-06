package com.lvonasek.arcore3dscanner.ui;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Handler;
import android.util.LruCache;
import android.widget.ImageView;
import java.io.File;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

public class ThumbnailLoaderTest {
  private static void check(boolean value, String message) {
    if (!value) throw new AssertionError(message);
  }
  private static Object field(Object owner, String name) throws Exception {
    Field field = owner.getClass().getDeclaredField(name);
    field.setAccessible(true);
    return field.get(owner);
  }
  private static void await(BooleanSupplier condition) throws Exception {
    long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
    while (!condition.getAsBoolean()) {
      Handler.drain();
      if (System.nanoTime() > deadline) throw new AssertionError("Timed out");
      Thread.sleep(1);
    }
    Handler.drain();
  }
  private static void gate(CountDownLatch latch) {
    try { check(latch.await(5, TimeUnit.SECONDS), "Gate timed out"); }
    catch (InterruptedException ignored) { /* Simulate a non-interruptible decoder. */ }
  }
  private static Bitmap image(File file) { return new Bitmap(256, 256, file.getAbsolutePath()); }

  private static void stop(ThumbnailLoader loader) throws Exception {
    loader.close();
    ThreadPoolExecutor workers = (ThreadPoolExecutor) field(loader, "workers");
    await(workers::isTerminated);
  }

  private static void boundedQueueAndCache() throws Exception {
    CountDownLatch release = new CountDownLatch(1);
    AtomicInteger calls = new AtomicInteger(), active = new AtomicInteger(), peak = new AtomicInteger();
    ThumbnailLoader loader = new ThumbnailLoader(file -> {
      int n = active.incrementAndGet(); peak.accumulateAndGet(n, Math::max);
      calls.incrementAndGet(); gate(release); active.decrementAndGet(); return image(file);
    }, 2, 3, 2 * 256 * 256 * 4);
    List<ImageView> views = new ArrayList<>();
    try {
      for (int i = 0; i < 100; i++) {
        ImageView view = new ImageView(); views.add(view);
        loader.bind(view, new File("/scans/" + i + ".obj"));
      }
      ThreadPoolExecutor workers = (ThreadPoolExecutor) field(loader, "workers");
      check(workers.getQueue().size() <= 3, "Queue exceeded capacity");
      check(((Map<?, ?>) field(loader, "jobs")).size() <= 5, "In-flight work is unbounded");
      release.countDown();
      await(() -> views.stream().allMatch(view -> view.bitmap != null));
      check(calls.get() == 100, "Dropped or repeated requests under pressure");
      check(peak.get() <= 2, "Too many decoding workers");
      check(((LruCache<?, ?>) field(loader, "cache")).size() <= 2 * 256 * 256 * 4,
              "Cache exceeded memory budget");
      for (ImageView view : views) check(!view.bitmap.recycled, "Eviction recycled a displayed bitmap");
      @SuppressWarnings("unchecked")
      LruCache<String, ?> cache = (LruCache<String, ?>) field(loader, "cache");
      Bitmap evicted = null;
      for (ImageView view : views) if (cache.get(view.bitmap.label) == null) { evicted = view.bitmap; break; }
      check(evicted != null, "Memory pressure didn't evict anything");
      loader.bind(new ImageView(), new File(evicted.label));
      await(() -> calls.get() == 101);
    } finally { release.countDown(); stop(loader); }
  }

  private static void coalescingAndHits() throws Exception {
    AtomicInteger calls = new AtomicInteger();
    CountDownLatch release = new CountDownLatch(1);
    ThumbnailLoader loader = new ThumbnailLoader(file -> {
      calls.incrementAndGet(); gate(release); return image(file);
    }, 2, 3, 1024 * 1024);
    try {
      ImageView first = new ImageView(), second = new ImageView(), third = new ImageView();
      File file = new File("relative/same.obj");
      loader.bind(first, file); loader.bind(second, file);
      await(() -> calls.get() == 1);
      check(((Map<?, ?>) field(loader, "jobs")).size() == 1, "Duplicate decode was queued");
      release.countDown(); await(() -> first.bitmap != null && second.bitmap != null);
      check(first.bitmap == second.bitmap, "Coalesced result wasn't shared");
      check(first.bitmap.label.equals(file.getAbsolutePath()), "Request path wasn't captured absolute");
      loader.bind(third, file);
      check(third.bitmap == first.bitmap && calls.get() == 1, "Cache hit submitted work");
      check(((Map<?, ?>) field(loader, "jobs")).isEmpty(), "Cache hit created a job");
    } finally { release.countDown(); stop(loader); }
  }

  private static void recycledRowsAndFolders() throws Exception {
    CountDownLatch release = new CountDownLatch(1);
    AtomicInteger oldCalls = new AtomicInteger();
    List<Bitmap> oldImages = new ArrayList<>();
    ThumbnailLoader loader = new ThumbnailLoader(file -> {
      if (file.getParent().equals("/old")) {
        oldCalls.incrementAndGet(); gate(release);
        Bitmap bitmap = image(file);
        synchronized (oldImages) { oldImages.add(bitmap); }
        return bitmap;
      }
      return image(file);
    }, 2, 3, 1024 * 1024);
    try {
      ImageView row = new ImageView();
      loader.bind(row, new File("/old/same.obj"));
      await(() -> oldCalls.get() == 1);
      loader.bind(row, new File("/new/same.obj"));
      await(() -> row.bitmap != null);
      check(row.bitmap.label.equals("/new/same.obj"), "Basename collided across folders");
      release.countDown();
      await(() -> {
        synchronized (oldImages) { return !oldImages.isEmpty(); }
      });
      await(() -> ((ThreadPoolExecutor) uncheckedField(loader, "workers")).getActiveCount() == 0);
      check(row.bitmap.label.equals("/new/same.obj"), "Recycled row received an old result");
      loader.clear();
      row.bitmap = null;
      loader.bind(row, null);
      check(((Map<?, ?>) field(loader, "bindings")).isEmpty(), "Folder placeholder retained a binding");
    } finally { release.countDown(); stop(loader); }
  }

  private static void cancelQueuedAndRebindDuringClear() throws Exception {
    CountDownLatch release = new CountDownLatch(1);
    AtomicInteger calls = new AtomicInteger();
    List<String> decoded = java.util.Collections.synchronizedList(new ArrayList<>());
    ThumbnailLoader loader = new ThumbnailLoader(file -> {
      calls.incrementAndGet(); decoded.add(file.getName()); gate(release); return image(file);
    }, 1, 1, 1024 * 1024);
    try {
      ImageView busy = new ImageView(), recycled = new ImageView(), fresh = new ImageView();
      loader.bind(busy, new File("/old/busy.obj"));
      await(() -> calls.get() == 1);
      loader.bind(recycled, new File("/old/obsolete.obj"));
      loader.bind(recycled, null);
      check(((ThreadPoolExecutor) field(loader, "workers")).getQueue().isEmpty(),
              "Recycling into a folder didn't remove queued work");
      loader.clear();
      loader.bind(recycled, new File("/new/first.obj"));
      loader.bind(fresh, new File("/new/second.obj")); // Old decoder still occupies the only worker.
      release.countDown();
      await(() -> recycled.bitmap != null && fresh.bitmap != null);
      check(busy.bitmap == null, "Old generation reached its view");
      check(calls.get() == 3 && !decoded.contains("obsolete.obj"), "Rejected/deferred work was lost or stale work ran");
    } finally { release.countDown(); stop(loader); }
  }

  private static Object uncheckedField(Object owner, String name) {
    try { return field(owner, name); } catch (Exception error) { throw new AssertionError(error); }
  }

  private static void invalidationAndClose() throws Exception {
    for (boolean destroy : new boolean[]{false, true}) {
      CountDownLatch release = new CountDownLatch(1);
      AtomicInteger calls = new AtomicInteger();
      List<Bitmap> results = new ArrayList<>();
      ThumbnailLoader loader = new ThumbnailLoader(file -> {
        calls.incrementAndGet(); gate(release);
        Bitmap bitmap = image(file);
        synchronized (results) { results.add(bitmap); }
        return bitmap;
      }, 1, 2, 1024 * 1024);
      try {
        ImageView running = new ImageView(), queued = new ImageView();
        loader.bind(running, new File("/old/running.obj"));
        await(() -> calls.get() == 1);
        loader.bind(queued, new File("/old/queued.obj"));
        if (destroy) loader.close(); else loader.clear();
        release.countDown();
        await(() -> {
          synchronized (results) { return !results.isEmpty() && results.get(0).recycled; }
        });
        check(running.bitmap == null && queued.bitmap == null, "Stale result reached invalidated views");
        check(calls.get() == 1, "Invalidation didn't cancel queued work");
        check(((LruCache<?, ?>) field(loader, "cache")).size() == 0, "Stale result repopulated cache");
        ImageView fresh = new ImageView();
        loader.bind(fresh, new File("/old/running.obj"));
        if (destroy) {
          check(((Map<?, ?>) field(loader, "jobs")).isEmpty(), "Closed loader accepted work");
        } else {
          await(() -> fresh.bitmap != null);
          check(calls.get() == 2, "New generation wasn't loaded");
        }
      } finally { release.countDown(); stop(loader); }
    }
  }

  private static void missingAndBroken() throws Exception {
    AtomicInteger calls = new AtomicInteger();
    ThumbnailLoader loader = new ThumbnailLoader(file -> {
      calls.incrementAndGet();
      if (file.getName().equals("bad.obj")) throw new IllegalArgumentException("Broken file");
      return null;
    }, 1, 2, 2048);
    try {
      ImageView missing = new ImageView(), broken = new ImageView();
      loader.bind(missing, new File("/missing.obj")); loader.bind(broken, new File("/bad.obj"));
      await(() -> calls.get() == 2 && ((Map<?, ?>) uncheckedField(loader, "jobs")).isEmpty());
      loader.bind(missing, new File("/missing.obj")); loader.bind(broken, new File("/bad.obj"));
      check(calls.get() == 2, "Failed images were repeatedly decoded on bind");
      check(missing.bitmap == null && broken.bitmap == null, "Failed images overwrote placeholders");
      check(((LruCache<?, ?>) field(loader, "cache")).size() == 2048, "Misses weren't charged to cache");
    } finally { stop(loader); }
  }

  private static File fixture(File scan, String name, int width, int height) throws Exception {
    File file = new File(scan, name);
    Files.write(file.toPath(), new byte[]{9, 8, 7});
    BitmapFactory.images.put(file.getAbsolutePath(), new int[]{width, height});
    return file;
  }

  private static void allocationFailureRecovery() throws Exception {
    CountDownLatch release = new CountDownLatch(1);
    AtomicInteger calls = new AtomicInteger();
    List<Thread> decodingThreads = java.util.Collections.synchronizedList(new ArrayList<>());
    ThumbnailLoader loader = new ThumbnailLoader(file -> {
      decodingThreads.add(Thread.currentThread());
      if (calls.incrementAndGet() == 1) {
        gate(release);
        throw new OutOfMemoryError("Injected decoder exhaustion");
      }
      return image(file);
    }, 1, 1, 1024 * 1024);
    try {
      ImageView failed = new ImageView(), queued = new ImageView(), deferred = new ImageView();
      File retry = new File("/scans/retry.obj");
      loader.bind(failed, retry);
      await(() -> calls.get() == 1);
      loader.bind(queued, new File("/scans/good.obj"));
      loader.bind(deferred, new File("/scans/deferred.obj"));
      release.countDown();
      await(() -> queued.bitmap != null && deferred.bitmap != null
              && ((Map<?, ?>) uncheckedField(loader, "jobs")).isEmpty());
      check(failed.bitmap == null && calls.get() == 3, "Exhausted request leaked a result or admission slot");
      loader.bind(failed, retry);
      check(calls.get() == 3 && ((Map<?, ?>) field(loader, "jobs")).isEmpty(),
              "Allocation failure caused a same-generation retry loop");
      loader.clear();
      loader.bind(failed, retry);
      await(() -> failed.bitmap != null);
      check(calls.get() == 4 && failed.bitmap.label.equals(retry.getAbsolutePath()),
              "Failed path didn't recover after refresh");
      for (Thread thread : decodingThreads) check(thread == decodingThreads.get(0), "Exhaustion killed the worker");
    } finally { release.countDown(); stop(loader); }
  }

  private static void allocationFailureReleasesIntermediates(File root) throws Exception {
    File scan = new File(root, "exhausted.dataset"); check(scan.mkdir(), "Create exhaustion fixture");
    fixture(scan, "00000000.jpg", 1024, 2048);
    for (boolean existing : new boolean[]{false, true}) {
      if (existing) fixture(scan, "thumbnail.jpg", 180, 320);
      int before = Bitmap.allocated.size();
      Bitmap.exhaustScaling = true;
      try {
        try {
          check(ThumbnailLoader.decode(scan) == null, "Failed scaling published a bitmap");
        } catch (OutOfMemoryError expected) {
          check(existing, "Generation failed to handle scaling exhaustion"); // Job.run handles this branch.
        }
      } finally { Bitmap.exhaustScaling = false; }
      check(Bitmap.allocated.size() > before, "No intermediate allocated before injected failure");
      for (int i = before; i < Bitmap.allocated.size(); i++) {
        check(Bitmap.allocated.get(i).recycled, "Allocation failure leaked an intermediate bitmap");
      }
      check(scan.list().length == (existing ? 2 : 1), "Allocation failure left a partial cache file");
    }
  }

  private static void decodingAndAtomicFiles(File root) throws Exception {
    File scan = new File(root, "large.dataset"); check(scan.mkdir(), "Create fixture");
    File source = fixture(scan, "00000000.jpg", 8193, 16385);
    int before = Bitmap.allocated.size();
    Bitmap result = ThumbnailLoader.decode(scan);
    check(result != null && result.getWidth() == 256 && result.getHeight() == 256, "Incorrect crop/scale");
    check(BitmapFactory.maxDecodedDimension <= 512, "Full-resolution source decode");
    for (int i = before; i < Bitmap.allocated.size(); i++) {
      Bitmap bitmap = Bitmap.allocated.get(i);
      check(bitmap == result ? !bitmap.recycled : bitmap.recycled, "Intermediate bitmap leak or alias recycle");
    }
    check(Files.readAllBytes(source.toPath())[0] == 9, "Source scan was modified");
    check(new File(scan, "thumbnail.jpg").length() == 4, "Complete thumbnail wasn't published");
    check(scan.list().length == 2, "Temporary file leaked");

    File fail = new File(root, "failed.dataset"); check(fail.mkdir(), "Create failure fixture");
    fixture(fail, "00000000.jpg", 256, 256);
    Bitmap.failCompression = true;
    check(ThumbnailLoader.decode(fail) != null, "Compression failure lost in-memory thumbnail");
    Bitmap.failCompression = false;
    check(!new File(fail, "thumbnail.jpg").exists(), "Partial thumbnail was published");
    check(fail.list().length == 1, "Failed compression leaked temporary file");

    File existing = fixture(fail, "thumbnail.jpg", 256, 256);
    before = Bitmap.allocated.size();
    result = ThumbnailLoader.decode(fail);
    check(!result.recycled && Bitmap.allocated.size() == before + 1, "No-op transforms recycled their alias");
    check(Files.readAllBytes(existing.toPath())[0] == 9, "Existing thumbnail was overwritten");
    BitmapFactory.failPixels = true;
    check(ThumbnailLoader.decode(fail) == null, "Null pixel decode wasn't handled");
    BitmapFactory.failPixels = false;

    File corrupt = new File(root, "corrupt.obj"); check(corrupt.mkdir(), "Create corrupt fixture");
    Files.write(new File(corrupt, "thumbnail.jpg").toPath(), new byte[]{0});
    check(ThumbnailLoader.decode(corrupt) == null, "Invalid bounds weren't handled");
    check(corrupt.list().length == 1, "Corrupt thumbnail handling changed source files");
  }

  public static void main(String[] args) throws Exception {
    boundedQueueAndCache();
    coalescingAndHits();
    recycledRowsAndFolders();
    cancelQueuedAndRebindDuringClear();
    invalidationAndClose();
    missingAndBroken();
    allocationFailureRecovery();
    Handler.drain();
    allocationFailureReleasesIntermediates(new File(args[0]));
    decodingAndAtomicFiles(new File(args[0]));
    System.out.println("PASS: bounded queue/cache, coalescing, hits, path identity, recycling, lifecycle, OOME recovery, decode and atomic publication");
  }
}
