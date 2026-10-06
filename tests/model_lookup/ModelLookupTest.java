package com.lvonasek.arcore3dscanner.ui;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

public class ModelLookupTest {
  private static int checks;
  private static void expect(File expected, File actual) {
    checks++;
    if (expected == null ? actual != null : !expected.equals(actual))
      throw new AssertionError("Expected " + expected + ", got " + actual);
  }
  public static void main(String[] args) throws Exception {
    Path root = new File(args[0]).toPath();
    Path cloud = Files.createDirectory(root.resolve("cloud.ply"));
    Files.write(cloud.resolve("aaa-position.txt"), new byte[]{1});
    File points = Files.write(cloud.resolve("points.ply"), new byte[]{2}).toFile();
    expect(points, ModelLookup.find(cloud.toFile()));
    expect(points, ModelLookup.find(points));
    expect(null, ModelLookup.find(cloud.resolve("aaa-position.txt").toFile()));
    expect(null, ModelLookup.find(root.resolve("missing.obj").toFile()));
    expect(null, ModelLookup.find(Files.createDirectory(root.resolve("empty.obj")).toFile()));
    File preferred = Files.write(cloud.resolve("cloud.ply"), new byte[]{3}).toFile();
    expect(preferred, ModelLookup.find(cloud.toFile()));
    Path obj = Files.createDirectory(root.resolve("mesh.obj"));
    Files.write(obj.resolve("wrong.ply"), new byte[]{1});
    File model = Files.write(obj.resolve("model.obj"), new byte[]{1}).toFile();
    expect(model, ModelLookup.find(obj.toFile()));
    Path staging = Files.createDirectory(root.resolve(".import-stage"));
    File imported = Files.write(staging.resolve("imported.ply"), new byte[]{1}).toFile();
    expect(imported, ModelLookup.find(staging.toFile()));
    Path dataset = Files.createDirectory(root.resolve("raw.dataset"));
    Files.write(dataset.resolve("preview.obj"), new byte[]{1});
    expect(null, ModelLookup.find(dataset.toFile()));
    expect(null, ModelLookup.find(null));
    System.out.println("PASS: " + checks + " read-only model lookup checks");
  }
}
