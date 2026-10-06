package com.lvonasek.arcore3dscanner.ui;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public class StorageRootTest {
  public static void main(String[] args) throws Exception {
    Path root = new File(args[0]).toPath();
    File legacy = Files.createDirectory(root.resolve("legacy")).toFile();
    Path original = legacy.toPath().resolve("existing.obj");
    byte[] bytes = new byte[]{1, 2, 3};
    Files.write(original, bytes);
    File app = root.resolve("app/documents/scans").toFile();
    if (!StorageRoot.choose(legacy, app).equals(legacy.getCanonicalFile()))
      throw new AssertionError("Did not retain writable legacy library");
    if (!java.util.Arrays.equals(bytes, Files.readAllBytes(original)) || legacy.list().length != 1)
      throw new AssertionError("Probe modified scan data or left temporary files");
    File blocked = Files.write(root.resolve("blocked"), bytes).toFile();
    if (!StorageRoot.choose(blocked, app).equals(app.getCanonicalFile()))
      throw new AssertionError("App-storage fallback failed");
    File internal = root.resolve("internal/scans").toFile();
    if (!StorageRoot.choose(null, blocked, internal).equals(internal.getCanonicalFile()))
      throw new AssertionError("Internal fallback failed when external storage is absent");
    try {
      StorageRoot.choose(blocked);
      throw new AssertionError("Unwritable candidates reported success");
    } catch (IOException expected) {}
    if (!java.util.Arrays.equals(bytes, Files.readAllBytes(blocked.toPath())))
      throw new AssertionError("Failed probe modified existing file");
    if (app.list().length != 0 || internal.list().length != 0)
      throw new AssertionError("Fallback probe did not clean up");
    System.out.println("PASS: writable legacy selection, app/internal fallback, failed writes and data preservation");
  }
}
