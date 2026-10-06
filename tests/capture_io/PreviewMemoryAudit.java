package com.lvonasek.arcore3dscanner.diagnostics;

import java.io.*;
import java.util.*;

/** Read-only committed-preview header audit. Skips all vertex/image payloads. */
public class PreviewMemoryAudit {
  static void check(boolean value, String message) throws IOException {
    if (!value) throw new IOException(message);
  }
  static String state(File root) throws IOException {
    try (BufferedReader in = new BufferedReader(new FileReader(new File(root, "state.txt")))) {
      return in.readLine();
    }
  }
  public static void main(String[] args) {
    try {
      check(args.length == 1, "Expected capture directory");
      File root = new File(args[0]);
      String before = state(root);
      check(before != null, "Missing state");
      int count = Integer.parseInt(before.trim().split("\\s+")[0]);
      check(count > 0 && count <= 10000, "Invalid/bounded frame count");
      Map<String, long[]> meshes = new HashMap<>();
      for (int frame = 0; frame < count; frame++) {
        File file = new File(root, String.format(Locale.US, "%08d.bin", frame));
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
          int segments = Integer.reverseBytes(in.readInt());
          check(segments >= 0 && segments <= 8192 && 4L+12L*segments <= in.length(), "Invalid preview header");
          String[] keys = new String[segments];
          for (int i = 0; i < segments; i++) {
            int x = Integer.reverseBytes(in.readInt()), y = Integer.reverseBytes(in.readInt());
            int z = Integer.reverseBytes(in.readInt());
            keys[i] = x + "," + y + "," + z;
          }
          for (int i = 0; i < segments; i++) {
            long faces = Integer.reverseBytes(in.readInt()) & 0xffffffffL;
            long vertices = Integer.reverseBytes(in.readInt()) & 0xffffffffL;
            long bytes = vertices * 28L + faces * 12L;
            check(bytes <= in.length()-in.getFilePointer(), "Truncated preview payload");
            in.seek(in.getFilePointer() + bytes);
            if (vertices == 0) meshes.remove(keys[i]);
            else meshes.put(keys[i], new long[]{vertices, faces, bytes});
          }
          check(in.getFilePointer() == in.length(), "Unexpected preview suffix");
        }
        check(meshes.size() <= 32768, "Preview inventory limit exceeded");
      }
      check(before.equals(state(root)), "Capture changed during read-only audit");
      long vertices = 0, faces = 0, bytes = 0;
      for (long[] mesh : meshes.values()) { vertices += mesh[0]; faces += mesh[1]; bytes += mesh[2]; }
      System.out.printf(Locale.US, "committed_frames=%d mesh_segments=%d vertices=%d faces=%d preview_array_bytes=%d preview_array_MiB=%.3f%n",
              count, meshes.size(), vertices, faces, bytes, bytes / 1048576.0);
      System.out.println("Read-only header estimate; excludes container/GL/AR/export memory; no vertex or image payload read");
    } catch (Throwable error) { error.printStackTrace(System.err); System.exit(2); }
  }
}
