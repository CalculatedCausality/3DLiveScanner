package com.lvonasek.arcore3dscanner.main;

import android.util.Log;

import com.lvonasek.arcore3dscanner.ui.AbstractActivity;
import com.lvonasek.utils.IO;

import java.io.File;
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;

public class Exporter
{
  public static final String[] FILE_EXT = {".dataset", ".obj", ".ply"};
  public static final String EXT_DATASET = FILE_EXT[0];
  public static final String EXT_OBJ = FILE_EXT[1];
  public static final String EXT_PLY = FILE_EXT[2];

  public static final int EXPORT_TYPE_FLOORPLAN = -100;
  public static final int EXPORT_TYPE_POINTCLOUD = -200;

  public static String compressModel(File model2share) {
    File staging = null;
    try {
      // Separate requests must not replace an archive another share intent is still reading.
      staging = IO.createStagingDirectory(AbstractActivity.getScratchPath());
      File zipFile = new File(staging, "upload.scan.zip");
      if (model2share.getCanonicalFile().equals(new File(AbstractActivity.getPath(false)).getCanonicalFile())) {
        // Legacy loose-model viewer fallback: do not include other scans or the cache directory.
        File[] files = model2share.listFiles();
        if (files == null) throw new IOException("Unable to list " + model2share);
        ArrayList<String> filesToZip = new ArrayList<>();
        for (File file : files) if (file.isFile()) filesToZip.add(file.getAbsolutePath());
        IO.zip(filesToZip, zipFile.getAbsolutePath());
      } else {
        IO.zipDirectory(model2share, zipFile.getAbsolutePath());
      }
      return zipFile.getAbsolutePath();
    } catch (Exception e) {
      if (staging != null) IO.deleteRecursive(staging);
      throw new IllegalStateException("Unable to archive " + model2share, e);
    }
  }

  /** Publishes the existing scan-directory format in one rename and retains all source files.
   * Returns the model inside the directory, as expected by the viewer. Existing scans are refused.
   * Callers must catch IllegalStateException before resetting the service or deleting its source. */
  public static File export(File file, String filename) {
    File staging = null;
    try {
      int type = getModelType(file.getName());
      if (type != 1 && type != 2) throw new IOException("Unsupported model: " + file);
      if (filename == null || filename.isEmpty() || !new File(filename).getName().equals(filename)) {
        throw new IOException("Invalid model name: " + filename);
      }
      File root = new File(AbstractActivity.getPath(false));
      File destination = IO.resolveContainedFile(root, filename + FILE_EXT[type]);
      if (destination.exists()) throw new IOException("Scan already exists: " + destination);
      staging = IO.createStagingDirectory(root);
      copyResources(file, staging);
      String modelName = filename + FILE_EXT[type];
      File model = IO.resolveContainedFile(staging, modelName);
      if (model.exists()) throw new IOException("Model/resource name collision: " + modelName);
      IO.copyChecked(file, model);
      if (destination.exists() || !staging.renameTo(destination)) {
        throw new IOException("Unable to publish scan: " + destination);
      }
      return new File(destination, modelName);
    } catch (IOException e) {
      throw new IllegalStateException("Unable to export " + file, e);
    } finally {
      if (staging != null) IO.deleteRecursive(staging);
    }
  }

  private static void copyResources(File model, File staging) throws IOException {
    File source = model.getAbsoluteFile().getParentFile();
    ArrayList<String> resources = model.getName().endsWith(EXT_OBJ)
            ? readObjResources(model) : new ArrayList<>();
    if (new File(source, "position.txt").exists()) resources.add("position.txt");
    for (String name : resources) {
      File input = IO.resolveContainedFile(source, name);
      File output = IO.resolveContainedFile(staging, name);
      if (!output.getParentFile().isDirectory() && !output.getParentFile().mkdirs()) {
        throw new IOException("Unable to create " + output.getParent());
      }
      IO.copyChecked(input, output);
    }
  }

  public static boolean isFolder(String s) {
    return getModelType(s) < 0;
  }

  public static int getModelType(String filename) {
    for(int i = 0; i < FILE_EXT.length; i++) {
      if (filename.endsWith(FILE_EXT[i]))
        return i;
    }
    return -1;
  }

  public static String getMtlResource(String obj)
  {
    try {
      return readMtlResource(new File(obj));
    } catch (IOException e) {
      // Thumbnail callers use a nullable lookup. Export uses the strict reader directly.
      Log.e(AbstractActivity.TAG, "Unable to read model resources: " + obj, e);
      return null;
    }
  }

  public static ArrayList<String> getObjResources(File file)
  {
    try {
      return readObjResources(file);
    } catch (IOException e) {
      throw new IllegalStateException("Unable to read model resources: " + file, e);
    }
  }

  private static String readMtlResource(File obj) throws IOException {
    try (BufferedReader reader = new BufferedReader(new FileReader(obj))) {
      String line;
      while ((line = reader.readLine()) != null) {
        line = line.trim();
        if (line.equals("mtllib")) throw new IOException("Missing material library in " + obj);
        if (line.startsWith("mtllib ") || line.startsWith("mtllib\t")) {
          String name = line.substring(6).trim();
          IO.resolveContainedFile(obj.getAbsoluteFile().getParentFile(), name);
          return name;
        }
      }
    }
    return null;
  }

  private static ArrayList<String> readObjResources(File file) throws IOException {
    HashSet<String> files = new HashSet<>();
    ArrayList<String> output = new ArrayList<>();
    File root = file.getAbsoluteFile().getParentFile();
    String mtlLib = readMtlResource(file);
    if (mtlLib != null) {
      output.add(mtlLib);
      // Generated preview is optional; declared material libraries and textures are not.
      if (IO.resolveContainedFile(root, mtlLib + ".png").isFile()) output.add(mtlLib + ".png");
      try (BufferedReader reader = new BufferedReader(new FileReader(IO.resolveContainedFile(root, mtlLib)))) {
        String line;
        while ((line = reader.readLine()) != null) {
          line = line.trim();
          if (line.startsWith("map_") || line.startsWith("norm ") || line.startsWith("norm\t")) {
            String[] parts = line.split("\\s+", 2);
            if (parts.length != 2) throw new IOException("Missing texture name in " + mtlLib);
            String filename = parts[1].trim();
            IO.resolveContainedFile(root, filename);
            if (files.add(filename)) {
              output.add(filename);
            }
          }
        }
      }
    }
    return output;
  }

  public static void makeStructure(String path) {
    // Read-only enumeration: AbstractActivity.listFiles also schedules background deletions.
    File[] files = new File(path).listFiles();
    if (files == null) return;
    ArrayList<String> models = new ArrayList<>();
    for (File file : files)
      if (file.isFile() && getModelType(file.getName()) >= 0)
        models.add(file.getAbsolutePath());
    Collections.sort(models, String::compareTo);

    //restructure models
    for (String s : models) {

      File model = new File(s);
      File staging = null;
      boolean retainStaging = false;
      try {
        staging = IO.createStagingDirectory(model.getAbsoluteFile().getParentFile());
        copyResources(model, staging);
        File movedModel = new File(staging, model.getName());
        if (movedModel.exists() || !model.renameTo(movedModel)) {
          throw new IOException("Unable to stage model: " + model);
        }
        retainStaging = true;
        if (!staging.renameTo(model)) {
          // Keep the only model copy if rollback itself fails. Never clean it as scratch data.
          retainStaging = !movedModel.renameTo(model);
          throw new IOException("Unable to restructure " + model + (retainStaging ? "; retained in " + staging : ""));
        }
        retainStaging = false;
      } catch (IOException e) {
        Log.e(AbstractActivity.TAG, "Unable to restructure " + model, e);
      } finally {
        if (staging != null && !retainStaging) IO.deleteRecursive(staging);
      }
    }
  }
}
