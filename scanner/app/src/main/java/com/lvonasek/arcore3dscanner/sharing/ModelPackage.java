package com.lvonasek.arcore3dscanner.sharing;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Read-only, dependency-scoped packaging. No Android dependencies and no library-folder fallback. */
public final class ModelPackage {
  private static final int MAX_FILES = 4096;
  private static final int MAX_LINE = 65536;
  private static final long MAX_BYTES = 2L * 1024 * 1024 * 1024;
  private static final long TIME_LIMIT_NANOS = 10L * 60 * 1000000000;

  public static final class DatasetException extends IOException {
    DatasetException() { super("Dataset folders must be exported to OBJ or PLY before sharing."); }
  }

  public static final class Result {
    public final File file;
    public final String mimeType;

    private Result(File file, String mimeType) {
      this.file = file;
      this.mimeType = mimeType;
    }
  }

  private static final class Asset {
    final File file;
    final long size;
    final long modified;

    Asset(File file) {
      this.file = file;
      size = file.length();
      modified = file.lastModified();
    }
  }

  private final long started = System.nanoTime();
  private final Map<String, Asset> assets = new LinkedHashMap<>();
  private File root;
  private long totalBytes;

  private ModelPackage() {}

  /**
   * selected is an explicit OBJ/PLY file, or an app model folder named *.obj / *.ply
   * containing exactly one corresponding model at its top level. cacheDir is app cache.
   * Each successful call creates a new immutable-by-convention cache file. Never removes
   * previous successful shares (recipients may still be reading them).
   */
  public static Result prepare(File selected, File cacheDir) throws IOException {
    return new ModelPackage().build(selected.getAbsoluteFile(), cacheDir);
  }

  private Result build(File selected, File cacheDir) throws IOException {
    String extension = extension(selected);
    if (extension.equals("dataset")) throw new DatasetException();
    if (!extension.equals("obj") && !extension.equals("ply"))
      throw new IOException("Select an OBJ or PLY model, not a library or Downloads folder.");
    File model = selected;
    if (selected.isDirectory()) {
      root = selected.getCanonicalFile();
      File[] children = root.listFiles();
      if (children == null) throw new IOException("Cannot read the selected model folder.");
      if (children.length > MAX_FILES) throw new IOException("Too many files in the selected model folder.");
      model = null;
      for (File child : children) {
        checkTime();
        if (extension(child).equals(extension) && child.isFile()) {
          if (model != null) throw new IOException("Multiple models in this folder; select the exact model file.");
          model = child;
        }
      }
      if (model == null) throw new IOException("No " + extension.toUpperCase(Locale.ROOT) + " model in the selected folder.");
    } else {
      root = selected.getParentFile().getCanonicalFile();
    }
    String modelName = add(model);
    if (assets.get(modelName).size == 0) throw new IOException("The selected model is empty.");
    if (extension.equals("obj")) readObj(model);

    File shares = new File(cacheDir, "model-shares");
    if (!shares.isDirectory() && !shares.mkdirs()) throw new IOException("Cannot create the model sharing cache.");
    // A private directory prevents any subsequent share from reusing the recipient's URI.
    File destination = new File(shares, UUID.randomUUID().toString());
    if (!destination.mkdir()) throw new IOException("Cannot create a unique model sharing folder.");
    String name = model.getName();
    File output = new File(destination, extension.equals("obj")
        ? name.substring(0, name.length() - 4) + ".zip" : name);
    boolean complete = false;
    try {
      if (extension.equals("obj")) {
        try (ZipOutputStream zip = new ZipOutputStream(new BufferedOutputStream(new FileOutputStream(output)))) {
          for (Map.Entry<String, Asset> entry : assets.entrySet()) {
            zip.putNextEntry(new ZipEntry(entry.getKey()));
            copy(entry.getValue(), zip);
            zip.closeEntry();
          }
        }
      } else {
        try (OutputStream stream = new BufferedOutputStream(new FileOutputStream(output))) {
          copy(assets.get(modelName), stream);
        }
      }
      complete = true;
      return new Result(output, extension.equals("obj") ? "application/zip" : "application/octet-stream");
    } finally {
      if (!complete) {
        // Only this operation's incomplete output; never a scan or previous successful share.
        output.delete();
        destination.delete();
      }
    }
  }

  private static String extension(File file) {
    String name = file.getName();
    return name.substring(name.lastIndexOf('.') + 1).toLowerCase(Locale.ROOT);
  }

  private void checkTime() throws IOException {
    if (Thread.currentThread().isInterrupted() || System.nanoTime() - started > TIME_LIMIT_NANOS)
      throw new IOException("Model sharing was interrupted or exceeded the 10-minute limit.");
  }

  private File scoped(File file) throws IOException {
    File normalized = new File(file.getAbsoluteFile().toURI().normalize());
    File canonical = file.getCanonicalFile();
    if (!canonical.getPath().startsWith(root.getPath() + File.separator)
        || !canonical.equals(normalized))
      throw new IOException("Asset is outside the selected model root or uses a symbolic link: " + file.getName());
    return canonical;
  }

  private File reference(File parent, String name) throws IOException {
    if (name.isEmpty() || name.startsWith("/") || name.contains("\\") || name.contains(":")
        || name.indexOf('\0') >= 0)
      throw new IOException("Asset path is not portable: " + name);
    return scoped(new File(parent, name));
  }

  private String add(File file) throws IOException {
    checkTime();
    file = scoped(file);
    if (!file.isFile() || !file.canRead()) throw new IOException("Missing or unreadable required asset: " + file.getName());
    String name = file.getPath().substring(root.getPath().length() + 1).replace(File.separatorChar, '/');
    if (name.contains("\\") || name.contains(":")) throw new IOException("Asset path is not portable: " + name);
    if (!assets.containsKey(name)) {
      Asset asset = new Asset(file);
      if (assets.size() >= MAX_FILES || asset.size > MAX_BYTES - totalBytes)
        throw new IOException("Model exceeds the sharing limit (4096 files / 2 GiB).");
      totalBytes += asset.size;
      assets.put(name, asset);
    }
    return name;
  }

  private interface LineConsumer { void accept(String key, String value) throws IOException; }

  private void lines(File file, LineConsumer consumer) throws IOException {
    try (BufferedReader reader = new BufferedReader(new InputStreamReader(new FileInputStream(scoped(file)), StandardCharsets.UTF_8))) {
      StringBuilder line = new StringBuilder();
      int c;
      while ((c = reader.read()) != -1) {
        if (c == '\n') {
          consume(line.toString(), consumer);
          line.setLength(0);
          checkTime();
        } else {
          if (line.length() >= MAX_LINE) throw new IOException("Model metadata line exceeds 64 KiB.");
          line.append((char) c);
        }
      }
      consume(line.toString(), consumer);
    }
  }

  private static void consume(String line, LineConsumer consumer) throws IOException {
    line = line.trim();
    if (line.startsWith("\uFEFF")) line = line.substring(1).trim();
    if (line.isEmpty() || line.startsWith("#")) return;
    int i = 0;
    while (i < line.length() && !Character.isWhitespace(line.charAt(i))) i++;
    consumer.accept(line.substring(0, i).toLowerCase(Locale.ROOT), line.substring(i).trim());
  }

  private void readObj(File obj) throws IOException {
    LinkedHashSet<File> materials = new LinkedHashSet<>();
    boolean[] usesMaterial = {false};
    lines(obj, (key, value) -> {
      if (key.equals("mtllib")) {
        List<String> names = tokens(value);
        if (names.isEmpty()) throw new IOException("OBJ has an empty material-library reference.");
        // Some exporters emit unquoted names with spaces. Prefer that exact existing file.
        String joined = join(names);
        File whole = reference(obj.getParentFile(), joined);
        if (whole.isFile() && names.size() > 1 && value.indexOf('"') < 0 && value.indexOf('\'') < 0) {
          for (String name : names) {
            if (reference(obj.getParentFile(), name).exists())
              throw new IOException("Ambiguous material-library names; quote filenames containing spaces.");
          }
          names = java.util.Collections.singletonList(joined);
        }
        for (String name : names) {
          File mtl = reference(obj.getParentFile(), name);
          add(mtl);
          materials.add(mtl);
        }
      } else if (key.equals("usemtl")) {
        usesMaterial[0] = true;
      } else if (key.equals("call") || key.equals("shadow_obj") || key.equals("trace_obj") || key.equals("maplib")) {
        throw new IOException("Unsupported external OBJ dependency: " + key);
      }
    });
    if (usesMaterial[0] && materials.isEmpty()) throw new IOException("OBJ uses materials but has no material-library reference.");
    for (File mtl : materials) {
      lines(mtl, (key, value) -> {
        if (key.startsWith("map_") || key.equals("bump") || key.equals("disp")
            || key.equals("decal") || key.equals("refl") || key.equals("norm")) {
          add(reference(mtl.getParentFile(), textureName(value)));
        } else if (key.equals("ka") || key.equals("kd") || key.equals("ks")
            || key.equals("ke") || key.equals("tf")) {
          List<String> parts = tokens(value);
          if (!parts.isEmpty() && parts.get(0).equalsIgnoreCase("spectral")) {
            if (parts.size() < 2) throw new IOException("Missing spectral material asset.");
            add(reference(mtl.getParentFile(), parts.get(1)));
          }
        }
      });
    }
  }

  private static List<String> tokens(String text) throws IOException {
    List<String> result = new ArrayList<>();
    StringBuilder token = new StringBuilder();
    char quote = 0;
    for (int i = 0; i < text.length(); i++) {
      char c = text.charAt(i);
      if (quote != 0) {
        if (c == quote) quote = 0; else token.append(c);
      } else if (c == '#' && token.length() == 0) {
        break;
      } else if ((c == '"' || c == '\'') && token.length() == 0) {
        quote = c;
      } else if (Character.isWhitespace(c)) {
        if (token.length() > 0) { result.add(token.toString()); token.setLength(0); }
      } else token.append(c);
    }
    if (quote != 0) throw new IOException("Unclosed quote in an asset reference.");
    if (token.length() > 0) result.add(token.toString());
    return result;
  }

  private static String textureName(String value) throws IOException {
    List<String> parts = tokens(value);
    int i = 0;
    while (i < parts.size() && parts.get(i).startsWith("-")) {
      String option = parts.get(i++).toLowerCase(Locale.ROOT);
      if (option.equals("-o") || option.equals("-s") || option.equals("-t")) {
        int count = 0;
        while (i < parts.size() && count < 3) {
          try { Double.parseDouble(parts.get(i)); } catch (NumberFormatException e) { break; }
          i++; count++;
        }
        if (count == 0) throw new IOException("Missing texture option value: " + option);
      } else {
        int count;
        switch (option) {
          case "-mm": count = 2; break;
          case "-blendu": case "-blendv": case "-boost": case "-texres":
          case "-clamp": case "-bm": case "-imfchan": case "-type": case "-cc":
          case "-colorspace": count = 1; break;
          default: throw new IOException("Unsupported texture option: " + option);
        }
        i += count;
      }
    }
    if (i >= parts.size()) throw new IOException("Missing texture filename.");
    return join(parts.subList(i, parts.size()));
  }

  private static String join(List<String> parts) {
    StringBuilder result = new StringBuilder();
    for (String part : parts) {
      if (result.length() > 0) result.append(' ');
      result.append(part);
    }
    return result.toString();
  }

  private void copy(Asset asset, OutputStream output) throws IOException {
    File file = scoped(asset.file);
    verifyUnchanged(asset);
    long copied = 0;
    byte[] buffer = new byte[65536];
    try (FileInputStream input = new FileInputStream(file)) {
      int count;
      while ((count = input.read(buffer)) != -1) {
        checkTime();
        copied += count;
        if (copied > asset.size) throw new IOException("Model changed while preparing the share. Please try again.");
        output.write(buffer, 0, count);
      }
    }
    verifyUnchanged(asset);
    if (copied != asset.size) throw new IOException("Model changed while preparing the share. Please try again.");
  }

  private static void verifyUnchanged(Asset asset) throws IOException {
    if (!asset.file.isFile() || asset.file.length() != asset.size || asset.file.lastModified() != asset.modified)
      throw new IOException("Model changed while preparing the share. Please try again.");
  }
}
