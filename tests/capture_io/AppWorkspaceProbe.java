package com.lvonasek.arcore3dscanner.ui;

import com.lvonasek.utils.IO;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;

/** Android app_process probe under run-as UID. Synthetic file I/O, not AR capture. */
public class AppWorkspaceProbe {
  static void check(boolean value, String message) throws IOException {
    if (!value) throw new IOException(message);
  }
  static void write(File file, byte[] data, boolean sync) throws IOException {
    try (FileOutputStream descriptor = new FileOutputStream(file);
         BufferedOutputStream output = new BufferedOutputStream(descriptor, 256 * 1024)) {
      output.write(data);
      output.flush();
      if (sync) descriptor.getFD().sync();
    }
  }
  static double elapsed(long start) { return (System.nanoTime() - start) / 1e6; }
  static void measure(File root, byte[] image, byte[] preview, byte[] cloud, boolean parallel) throws Exception {
    check(root.mkdir(), "Cannot create isolated benchmark directory");
    ExecutorService writer = Executors.newSingleThreadExecutor();
    try {
      double[] times = new double[16];
      byte[] pose = new byte[432], timestamp = "12345.6789\n".getBytes("UTF-8");
      byte[] state = "16 360 640 180 320 500 500\n".getBytes("UTF-8");
      for (int frame = 0; frame < times.length; frame++) {
        String name = String.format(Locale.US, "%08d", frame);
        File jpg = new File(root, name + ".jpg"), mat = new File(root, name + ".mat");
        File tms = new File(root, name + ".tms"), bin = new File(root, name + ".bin");
        File pcl = new File(root, name + ".pcl");
        long started = System.nanoTime();
        Future<Boolean> imageWork = null;
        if (parallel) {
          imageWork = writer.submit(() -> {
            write(jpg, image, false); write(mat, pose, false); write(tms, timestamp, false);
            return true;
          });
        } else {
          write(jpg, image, false); write(mat, pose, false); write(tms, timestamp, false);
        }
        write(bin, preview, true);
        write(pcl, cloud, false);
        if (imageWork != null) check(imageWork.get(), "Image work failed");
        File temporary = new File(root, "state.txt.tmp");
        write(temporary, state, false);
        check(temporary.renameTo(new File(root, "state.txt")), "State publication failed");
        times[frame] = elapsed(started);
      }
      Arrays.sort(times);
      double sum = 0; for (double time : times) sum += time;
      System.out.printf(Locale.US, "frames=16 mean_ms=%.3f median_ms=%.3f p95_ms=%.3f%n",
              sum / times.length, times[8], times[14]);
    } finally {
      writer.shutdown();
      check(writer.awaitTermination(30, TimeUnit.SECONDS), "Writer did not finish");
      IO.deleteRecursive(root);
    }
  }
  public static void main(String[] args) throws Exception {
    try { run(args); }
    catch (Throwable error) { error.printStackTrace(System.err); System.exit(2); }
  }
  static void run(String[] args) throws Exception {
    check(args.length == 3 || (args.length == 4 && args[3].equals("serial")), "Expected private/library paths and unique probe ID");
    boolean parallel = args.length == 3;
    System.out.println("probe_uid=" + android.os.Process.myUid() + " synthetic_file_io=true parallel=" + parallel);
    check(args[2].matches("[a-f0-9]{32}"), "Invalid probe ID");
    String id = ".workspace-probe-" + args[2];
    File internal = new File(args[0], id), library = new File(args[1], id);
    check(internal.mkdir(), "Cannot create private scratch");
    try {
      boolean sharedAvailable = library.mkdir();
      if (!sharedAvailable)
        System.out.println("SKIP shared-storage comparison: this process cannot create library scratch");
      try {
        byte[] image = new byte[95136], preview = new byte[1312004], cloud = new byte[147460];
        Random random = new Random(79);
        random.nextBytes(image); random.nextBytes(preview); random.nextBytes(cloud);
        // ABBA controls ordering; all files are generated, with no captured input.
        File[] runs = sharedAvailable ? new File[]{library, internal, internal, library}
                                     : new File[]{internal, internal};
        for (File base : runs) {
          System.out.println("storage=" + (base.equals(internal) ? "internal" : "shared"));
          measure(new File(base, "run"), image, preview, cloud, parallel);
        }
        File outputRoot = sharedAvailable ? library : internal;
        File capture = new File(internal, "capture"), old = new File(outputRoot, "dataset");
        CaptureWorkspace workspace = new CaptureWorkspace(capture, old, true);
        check(workspace.select().equals(capture), "Wrong new capture selection");
        check(capture.mkdir(), "Cannot create synthetic capture");
        write(new File(capture, "state.txt"), "fixture-only".getBytes("UTF-8"), true);
        write(new File(capture, "00000000.pcl"), cloud, true);
        File destination = new File(outputRoot, "saved.dataset");
        long start = System.nanoTime();
        IO.publishDirectoryChecked(capture, destination);
        System.out.printf(Locale.US, "verified_publication_ms=%.3f cross_filesystem=%s%n", elapsed(start), sharedAvailable);
        check(new File(capture, "state.txt").isFile(), "Source removed before caller cleanup");
        check(new File(destination, "00000000.pcl").length() == cloud.length, "Bad published payload");
        check(workspace.select().equals(capture), "Lost interrupted private capture");
        check(old.mkdir(), "Cannot create legacy fixture");
        write(new File(old, "state.txt"), "old".getBytes("UTF-8"), false);
        check(workspace.select().equals(old), "Legacy capture was hidden");
        System.out.println("PASS selection and verified publication; source retained");
      } finally { if (sharedAvailable) IO.deleteRecursive(library); }
    } finally { IO.deleteRecursive(internal); }
  }
}
