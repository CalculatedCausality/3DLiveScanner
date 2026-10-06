package com.lvonasek.arcore3dscanner.ui;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/** Pick a writable library without moving or deleting any existing scan. */
final class StorageRoot {
  private StorageRoot() {}

  static File choose(File... candidates) throws IOException {
    IOException lastFailure = null;
    for (File candidate : candidates) {
      if (candidate == null) continue;
      File probe = null;
      try {
        if (!candidate.isDirectory() && !candidate.mkdirs())
          throw new IOException("Unable to create library " + candidate);
        // canWrite() alone does not establish that scoped-storage writes succeed.
        probe = File.createTempFile(".scanner-write-", ".tmp", candidate);
        try (FileOutputStream out = new FileOutputStream(probe)) { out.write(0); }
        return candidate.getCanonicalFile();
      } catch (IOException | SecurityException failure) {
        lastFailure = new IOException("Library is not writable: " + candidate, failure);
      } finally {
        if (probe != null) probe.delete();
      }
    }
    throw new IOException("No writable scan library", lastFailure);
  }
}
