package com.lvonasek.gles;

import java.util.Arrays;
import java.util.Locale;

/** Optional, bounded CPU-side frame timings. No logging or allocation per frame. */
public final class FrameTimings {
  private static final int CAPACITY = 256;
  // Callback, native draw, EGL swap, start interval, outside-frame gap, mode, swap error.
  private final long[][] samples = new long[CAPACITY][7];
  private int count, cursor, previousMode = -1;
  private long total, previousStart, previousEnd;
  private long nativeStart = -1, nativeEnd = -1;
  private boolean capturing;

  public synchronized void reset() {
    count = cursor = 0;
    total = previousStart = previousEnd = 0;
    nativeStart = nativeEnd = -1;
    previousMode = -1;
  }

  /** Called by the renderer; timestamps bracket the JNI call, including its gate. */
  public synchronized void recordNativeDraw(long start, long end, boolean scanning) {
    nativeStart = start;
    nativeEnd = end;
    capturing = scanning;
  }

  /** Called after swap, on the same GL thread as recordNativeDraw. */
  public synchronized void recordFrame(long start, long drawEnd, long swapEnd, boolean swapOk) {
    boolean hasNative = nativeStart >= start && nativeEnd >= nativeStart && nativeEnd <= drawEnd;
    int mode = hasNative ? (capturing ? 1 : 0) : 2;
    long[] sample = samples[cursor];
    sample[0] = drawEnd - start;
    sample[1] = hasNative ? nativeEnd - nativeStart : -1;
    sample[2] = swapEnd - drawEnd;
    // Do not combine scanning/preview transitions or the first frame after reset.
    boolean continuous = count > 0 && previousMode == mode;
    sample[3] = continuous ? start - previousStart : -1;
    sample[4] = continuous ? start - previousEnd : -1;
    sample[5] = mode;
    sample[6] = swapOk ? 0 : 1;
    previousStart = start;
    previousEnd = swapEnd;
    previousMode = mode;
    nativeStart = nativeEnd = -1;
    cursor = (cursor + 1) % CAPACITY;
    count = Math.min(CAPACITY, count + 1);
    total++;
  }

  /** Copy under the short writer lock, then calculate percentiles outside it. */
  public String snapshot() {
    long[][] copy;
    long frames, last;
    synchronized (this) {
      copy = new long[count][];
      for (int i = 0; i < count; ++i) copy[i] = samples[i].clone();
      frames = total;
      last = previousEnd;
    }
    StringBuilder out = new StringBuilder(4096);
    out.append("{\"capacity\":").append(CAPACITY).append(",\"frames_total\":").append(frames);
    out.append(",\"last_frame_age_ms\":").append(last == 0 ? -1 : (System.nanoTime() - last) / 1000000);
    out.append(",\"all\":");
    group(out, copy, -1);
    out.append(",\"capturing\":");
    group(out, copy, 1);
    out.append(",\"preview\":");
    group(out, copy, 0);
    return out.append('}').toString();
  }

  private static void group(StringBuilder out, long[][] copy, int mode) {
    int frames = 0, errors = 0;
    for (long[] sample : copy) if (mode < 0 || sample[5] == mode) {
      frames++;
      errors += sample[6];
    }
    out.append("{\"frames\":").append(frames).append(",\"swap_errors\":").append(errors);
    String[] names = {"callback_ms", "native_draw_ms", "egl_swap_ms", "start_interval_ms", "outside_frame_ms", "java_other_ms"};
    for (int column = 0; column < names.length; ++column) {
      long[] values = new long[frames];
      int n = 0;
      for (long[] sample : copy) if (mode < 0 || sample[5] == mode) {
        long value = column == 5 ? (sample[1] < 0 ? -1 : sample[0] - sample[1]) : sample[column];
        if (value >= 0) values[n++] = value;
      }
      out.append(",\"").append(names[column]).append("\":");
      statistics(out, values, n);
    }
    out.append('}');
  }

  private static void statistics(StringBuilder out, long[] values, int n) {
    out.append("{\"samples\":").append(n);
    if (n > 0) {
      Arrays.sort(values, 0, n);
      double sum = 0;
      int over50 = 0, over100 = 0;
      for (int i = 0; i < n; ++i) {
        sum += values[i];
        if (values[i] > 50000000) over50++;
        if (values[i] > 100000000) over100++;
      }
      out.append(String.format(Locale.ROOT,
          ",\"mean\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"over_50ms\":%d,\"over_100ms\":%d",
          sum / n / 1000000, values[(n - 1) / 2] / 1000000.0,
          values[(int)Math.ceil(n * .95) - 1] / 1000000.0,
          values[(int)Math.ceil(n * .99) - 1] / 1000000.0,
          values[n - 1] / 1000000.0, over50, over100));
    }
    out.append('}');
  }
}
