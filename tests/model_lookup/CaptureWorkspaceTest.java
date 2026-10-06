package com.lvonasek.arcore3dscanner.ui;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

public class CaptureWorkspaceTest {
  static void check(boolean condition, String message) {
    if (!condition) throw new AssertionError(message);
  }
  public static void main(String[] args) throws Exception {
    Path root = Files.createTempDirectory(new File(args[0]).toPath(), "capture-");
    File internal = root.resolve("private/dataset").toFile();
    File legacy = root.resolve("library/dataset").toFile();
    CaptureWorkspace modern = new CaptureWorkspace(internal, legacy, true);
    CaptureWorkspace old = new CaptureWorkspace(internal, legacy, false);
    check(modern.select().equals(internal), "Modern new capture must be internal");
    check(old.select().equals(legacy), "Legacy new capture must retain its path");
    check(!internal.exists() && !legacy.exists(), "Selection must not create/move files");
    Files.createDirectories(legacy.toPath());
    Files.write(legacy.toPath().resolve("state.txt"), new byte[]{1});
    check(modern.select().equals(legacy), "Upgrade hid existing/corrupt public capture");
    Files.createDirectories(internal.toPath());
    Files.write(internal.toPath().resolve("state.txt"), new byte[]{2});
    check(modern.select().equals(legacy), "Both captures must be recovered deterministically");
    check(Files.readAllBytes(internal.toPath().resolve("state.txt"))[0] == 2, "Changed other capture");
    Files.delete(legacy.toPath().resolve("state.txt"));
    check(modern.select().equals(internal), "Internal recovery was not rediscovered");
    check(old.select().equals(internal), "Legacy build hid a pending modern capture");
    Files.delete(internal.toPath().resolve("state.txt"));
    for (String name : new String[]{"00000000.pcl", "00000000.mat", "00000000.jpg",
                                   "00000000.tms", "00000000.bin",
                                   "state.txt.tmp", "model.obj", "model.ply"}) {
      File frame = new File(legacy, name);
      check(frame.createNewFile(), "Unable to create fixture");
      check(modern.select().equals(legacy), "Lost partial capture: " + name);
      check(frame.delete(), "Unable to remove fixture");
    }
    Files.write(legacy.toPath().resolve("thumb.png"), new byte[]{3});
    Files.write(legacy.toPath().resolve("test.txt"), new byte[]{4});
    check(modern.select().equals(internal), "Old viewer scratch must not force slow capture");
    File inaccessible = new File(root.toFile(), "inaccessible") {
      @Override public boolean exists() { return true; }
      @Override public File[] listFiles() { return null; }
    };
    check(new CaptureWorkspace(internal, inaccessible, true).select().equals(inaccessible),
          "Unreadable is not empty");
    File malformed = root.resolve("capture-file").toFile();
    Files.write(malformed.toPath(), new byte[]{5});
    check(new CaptureWorkspace(internal, malformed, true).select().equals(malformed),
          "Malformed working storage must remain discoverable for recovery");
    System.out.println("PASS: modern/legacy selection, both recovery roots, partial captures, no destructive selection");
  }
}
