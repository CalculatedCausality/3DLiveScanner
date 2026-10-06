package com.lvonasek.arcore3dscanner.ui;
import java.io.File;
public class AbstractActivity {
  public static File getModel(File scan) { return new File(scan, "model.obj"); }
}
