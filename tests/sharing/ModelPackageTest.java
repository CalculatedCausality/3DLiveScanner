package com.lvonasek.arcore3dscanner.sharing;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.ZipFile;

/** Dependency-free regression suite; every fixture lives in a temporary directory. */
public final class ModelPackageTest {
  private static int assertions;
  private static Path workspace;
  private static File cache;

  private static File write(String name, String contents) throws IOException {
    Path path = workspace.resolve(name);
    Files.createDirectories(path.getParent());
    Files.write(path, contents.getBytes(StandardCharsets.UTF_8));
    return path.toFile();
  }

  private static void check(boolean condition, String message) {
    assertions++;
    if (!condition) throw new AssertionError(message);
  }

  private static Map<String, byte[]> contents(File file) throws IOException {
    Map<String, byte[]> result = new LinkedHashMap<>();
    try (ZipFile zip = new ZipFile(file)) {
      java.util.Enumeration<? extends java.util.zip.ZipEntry> entries = zip.entries();
      while (entries.hasMoreElements()) {
        java.util.zip.ZipEntry entry = entries.nextElement();
        check(!entry.getName().startsWith("/") && !entry.getName().contains(".."), "Portable ZIP entry");
        result.put(entry.getName(), zip.getInputStream(entry).readAllBytes());
      }
    }
    return result;
  }

  private static void fails(File selected, String message) throws IOException {
    try {
      ModelPackage.prepare(selected, cache);
      throw new AssertionError("Expected failure: " + message);
    } catch (IOException e) {
      check(e.getMessage().contains(message), "Expected '" + message + "', got " + e);
    }
  }

  public static void main(String[] args) throws Exception {
    workspace = Files.createTempDirectory("model-sharing-test-");
    cache = workspace.resolve("cache").toFile();
    try {
      File obj = write("library/chair.obj/chair.obj", "mtllib materials/base.mtl second.mtl\nmtllib materials/base.mtl\nusemtl wood\nv 0 0 0\n");
      write("library/chair.obj/materials/base.mtl", "newmtl wood\nmap_Kd -s 1 1 1 ../textures/wood grain.png\nbump -bm 0.5 ../textures/normal.png\nKd spectral ../textures/wood.rfl 1\n");
      write("library/chair.obj/second.mtl", "newmtl other\nmap_Kd \"textures/wood grain.png\"\n");
      write("library/chair.obj/textures/wood grain.png", "texture bytes");
      write("library/chair.obj/textures/normal.png", "normal bytes");
      write("library/chair.obj/textures/wood.rfl", "spectral bytes");
      write("library/chair.obj/thumbnail.jpg", "private thumbnail");
      write("library/chair.obj/position.txt", "private GPS");
      write("library/chair.obj/unrelated.obj", "another model");
      write("library/other.obj", "other scan");
      ModelPackage.Result first = ModelPackage.prepare(obj, cache);
      check(first.mimeType.equals("application/zip"), "OBJ MIME type");
      Map<String, byte[]> entries = contents(first.file);
      check(entries.keySet().equals(new java.util.LinkedHashSet<>(Arrays.asList(
          "chair.obj", "materials/base.mtl", "second.mtl", "textures/wood grain.png", "textures/normal.png", "textures/wood.rfl"))), "Exact deduplicated dependency closure, no GPS/thumbnail/other scan");
      for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
        check(Arrays.equals(entry.getValue(), Files.readAllBytes(obj.toPath().getParent().resolve(entry.getKey()))), "Bytes and relative names preserved: " + entry.getKey());
      }
      byte[] snapshot = Files.readAllBytes(first.file.toPath());
      File second = ModelPackage.prepare(obj, cache).file;
      check(!first.file.equals(second), "Unique output for every share");
      write("library/chair.obj/textures/normal.png", "changed source");
      check(Arrays.equals(snapshot, Files.readAllBytes(first.file.toPath())), "Later shares/source edits cannot mutate prior share");
      check(first.file.toPath().startsWith(cache.toPath().resolve("model-shares")), "Output scoped to cache provider path");

      File ply = write("cloud.ply/cloud.ply", "ply\nformat ascii 1.0\nelement vertex 1\nend_header\n1 2 3\n");
      ModelPackage.Result cloud = ModelPackage.prepare(ply.getParentFile(), cache);
      check(cloud.file.getName().endsWith(".ply"), "PLY remains a direct model file");
      check(cloud.mimeType.equals("application/octet-stream"), "PLY compatible MIME");
      check(Arrays.equals(Files.readAllBytes(ply.toPath()), Files.readAllBytes(cloud.file.toPath())), "PLY contents intact");
      File solo = write("single.obj/model.obj", "v 0 0 0\n");
      check(contents(ModelPackage.prepare(solo.getParentFile(), cache).file).size() == 1, "App model-folder selection supported");
      File caps = write("UPPER.OBJ", "v 0 0 0\n");
      check(ModelPackage.prepare(caps, cache).mimeType.equals("application/zip"), "Case-insensitive extension");
      File spaced = write("spaced.obj/model.obj", "mtllib wood material.mtl\n");
      write("spaced.obj/wood material.mtl", "newmtl wood\n");
      check(contents(ModelPackage.prepare(spaced, cache).file).containsKey("wood material.mtl"), "Unquoted exporter material filename with spaces");
      write("spaced.obj/wood", "newmtl other\n");
      fails(spaced, "Ambiguous material-library names");
      write("spaced.obj/material.mtl", "newmtl another\n");
      write("spaced.obj/model.obj", "mtllib \"wood\" \"material.mtl\"\n");
      Map<String, byte[]> quoted = contents(ModelPackage.prepare(spaced, cache).file);
      check(quoted.containsKey("wood") && quoted.containsKey("material.mtl") && !quoted.containsKey("wood material.mtl"), "Quoted multiple libraries are not merged");

      fails(obj.getParentFile(), "Multiple models");
      fails(workspace.resolve("library").toFile(), "not a library");
      File dataset = workspace.resolve("raw.dataset").toFile();
      check(dataset.mkdir(), "Dataset fixture");
      fails(dataset, "exported to OBJ or PLY");
      fails(write("missing.obj", "mtllib missing.mtl\n"), "Missing or unreadable required asset");
      fails(write("material.obj", "usemtl wood\n"), "no material-library");
      fails(write("unsafe/model.obj", "mtllib ../outside.mtl\n"), "outside the selected model root");
      fails(write("unsafe/model.obj", "mtllib /etc/passwd\n"), "not portable");
      fails(write("unsafe/model.obj", "mtllib C:\\private.mtl\n"), "not portable");
      write("unsafe/material.mtl", "map_Kd ../../private.png\n");
      fails(write("unsafe/model.obj", "mtllib material.mtl\n"), "outside the selected model root");
      write("unsafe/material.mtl", "map_Kd absent.png\n");
      fails(workspace.resolve("unsafe/model.obj").toFile(), "Missing or unreadable required asset");
      write("unsafe/material.mtl", "map_Kd -mystery 1 texture.png\n");
      fails(workspace.resolve("unsafe/model.obj").toFile(), "Unsupported texture option");
      File outside = write("outside.mtl", "newmtl private\n");
      Files.createSymbolicLink(workspace.resolve("unsafe/link.mtl"), outside.toPath());
      fails(write("unsafe/model.obj", "mtllib link.mtl\n"), "symbolic link");
      fails(write("unsafe/model.obj", "call other.obj\n"), "Unsupported external OBJ dependency");
      fails(write("empty.obj", ""), "empty");
      fails(write("..\\escape.obj", "v 0 0 0\n"), "not portable");
      fails(write("long.obj", "v " + "1".repeat(65536)), "64 KiB");
      File huge = write("huge.ply", "ply");
      try (java.io.RandomAccessFile sparse = new java.io.RandomAccessFile(huge, "rw")) {
        sparse.setLength(2L * 1024 * 1024 * 1024 + 1);
      }
      fails(huge, "2 GiB");

      // A metadata-only list and small files exercise the dependency-count bound cheaply.
      StringBuilder many = new StringBuilder();
      for (int i = 0; i < 4096; i++) {
        write("many.obj/" + i + ".mtl", "newmtl material\n");
        many.append("mtllib ").append(i).append(".mtl\n");
      }
      fails(write("many.obj/model.obj", many.toString()), "4096 files");
      Thread.currentThread().interrupt();
      fails(solo, "interrupted");
      Thread.interrupted();
      File badCache = write("not-directory", "occupied");
      try {
        ModelPackage.prepare(solo, badCache);
        throw new AssertionError("Expected cache error");
      } catch (IOException expected) { check(expected.getMessage().contains("cache"), "Cache error surfaced"); }
      check(obj.isFile() && ply.isFile() && outside.isFile(), "Source models/assets never removed");
      System.out.println("ModelPackageTest: " + assertions + " assertions passed");
    } finally {
      try (java.util.stream.Stream<Path> paths = Files.walk(workspace)) {
        paths.sorted(java.util.Comparator.reverseOrder()).forEach(path -> {
          try { Files.delete(path); } catch (IOException e) { throw new RuntimeException(e); }
        });
      }
    }
  }
}
