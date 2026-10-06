package com.lvonasek.arcore3dscanner.ui;

import com.lvonasek.utils.IO;
import java.io.File;
import java.util.ArrayList;

/** Only the Android/UI boundary is replaced; IO and Exporter are production sources. */
public class AbstractActivity {
    public static final String TAG = "storage-test";
    public static File root;
    public static String getPath(boolean migrate) { return root.getAbsolutePath(); }
    public static File getTempPath() {
        File temp = new File(root, "cache");
        temp.mkdirs();
        return temp;
    }
    public static File getScratchPath() { return getTempPath(); }
    public static ArrayList<String> listFiles(File directory) {
        ArrayList<String> result = new ArrayList<>();
        File[] files = directory.listFiles();
        if (files != null) for (File file : files) result.add(file.getAbsolutePath());
        return result;
    }
    public static void deleteRecursive(File file) { IO.deleteRecursive(file); }
}
