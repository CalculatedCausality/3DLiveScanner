package com.lvonasek.arcore3dscanner.ui;

import java.io.File;
import java.util.Arrays;

/** Read-only lookup for the application's flat model-container format. */
final class ModelLookup {
  private ModelLookup() {}

  static File find(File source) {
    if (source == null) return null;
    if (source.isFile()) return extension(source) != null ? source : null;
    if (!source.isDirectory() || source.getName().endsWith(".dataset")) return null;
    String expected = extension(source);
    File namedModel = new File(source, source.getName());
    if (expected != null && namedModel.isFile()) return namedModel;
    File[] children = source.listFiles();
    if (children == null) return null;
    Arrays.sort(children);
    for (File child : children) {
      String type = extension(child);
      if (child.isFile() && type != null && (expected == null || expected.equals(type))) return child;
    }
    return null;
  }

  static String extension(File file) {
    String name = file.getName();
    if (name.endsWith(".obj")) return ".obj";
    if (name.endsWith(".ply")) return ".ply";
    return null;
  }
}
