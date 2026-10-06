package com.lvonasek.arcore3dscanner.ui;

import java.io.File;

/** Select only between owned working locations. Selection never moves/deletes data. */
final class CaptureWorkspace {
  private final File internal;
  private final File legacy;
  private final boolean preferInternal;

  CaptureWorkspace(File internal, File legacy, boolean preferInternal) {
    this.internal = internal;
    this.legacy = legacy;
    this.preferInternal = preferInternal;
  }

  File select() {
    // An upgrade must discover the existing public capture first. If both exist,
    // resolve it first, then discover the internal capture on the next refresh.
    if (hasCaptureData(legacy)) return legacy;
    if (hasCaptureData(internal)) return internal;
    return preferInternal ? internal : legacy;
  }

  static boolean hasCaptureData(File directory) {
    if (!directory.exists()) return false;
    File[] files = directory.listFiles();
    // Inaccessible or malformed working storage is not proof that it is empty.
    if (files == null) return true;
    for (File file : files) {
      String name = file.getName();
      if (name.equals("state.txt") || name.equals("state.txt.tmp")
          || name.equals("model.obj") || name.equals("model.ply")
          || name.matches("[0-9]{8}\\.(jpg|mat|tms|pcl|bin)")) return true;
    }
    return false;
  }
}
