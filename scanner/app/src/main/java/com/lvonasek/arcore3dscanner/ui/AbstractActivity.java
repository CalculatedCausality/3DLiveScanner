package com.lvonasek.arcore3dscanner.ui;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.Resources;
import android.graphics.Color;
import android.net.Uri;
import android.os.Environment;
import android.os.Bundle;
import android.os.Parcelable;
import android.preference.PreferenceManager;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

import com.lvonasek.arcore3dscanner.main.Exporter;
import com.lvonasek.utils.Compass;
import com.lvonasek.utils.Compatibility;
import com.lvonasek.utils.IO;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

public abstract class AbstractActivity extends Activity {
  protected static final String DELETE_POSTFIX = ".#$%";
  protected static final String FILE_KEY = "FILE2OPEN";
  protected static final String TEMP_DIRECTORY = "dataset";

  private static final String OLD_MODEL_DIRECTORY = "/Models/";
  private static final int PERMISSIONS_CODE = 1987;
  public static final String TAG = "arcore_app";

  private Compass mCompass;
  protected Runnable onPermissionFail = null;
  protected Runnable onPermissionSuccess = null;
  private static final ArrayList<File> toDelete = new ArrayList<>();
  private static final AtomicBoolean migrationActive = new AtomicBoolean(false);
  private static final AtomicBoolean restartApp = new AtomicBoolean(false);
  private static volatile File storageRoot;
  private static boolean appStorage;
  private static CaptureWorkspace captureWorkspace;
  private static volatile File captureRoot;
  private static File internalCaptureRoot;
  private static File scratchRoot;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    initializeStorage(getApplicationContext());
    initializeCaptureWorkspace(getApplicationContext());
  }

  private static synchronized void initializeStorage(Context context) {
    if (storageRoot != null) return;
    SharedPreferences preferences = PreferenceManager.getDefaultSharedPreferences(context);
    if ("quality".equals(com.lvonasek.arcore3dscanner.BuildConfig.FLAVOR)) {
      // The comparison app must never migrate, enumerate or write the working
      // scanner's public library. Its capture and library are independently owned.
      storageRoot = new File(context.getFilesDir(), "quality-scans");
      appStorage = true;
      storageRoot.mkdirs();
      return;
    }
    String savedPath = preferences.getString("SCANNER_LIBRARY_PATH", null);
    if (savedPath != null) {
      // A later permission/mount change must not silently hide the previous library.
      appStorage = preferences.getBoolean("SCANNER_LIBRARY_APP_STORAGE", true);
      storageRoot = new File(savedPath);
      return;
    }
    File publicLibrary = new File(Environment.getExternalStoragePublicDirectory(
            Environment.DIRECTORY_DOCUMENTS), "3D Live Scanner");
    File external = context.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS);
    File externalLibrary = external == null ? null : new File(external, "3D Live Scanner");
    try {
      File chosen = StorageRoot.choose(publicLibrary, externalLibrary,
              new File(context.getFilesDir(), "scans"));
      appStorage = !chosen.equals(publicLibrary.getCanonicalFile());
      storageRoot = chosen;
    } catch (IOException failure) {
      // Keep a stable app-owned location so existing failure/retry UI can handle
      // unavailable/full storage; never silently switch libraries mid-scan.
      appStorage = true;
      storageRoot = new File(context.getFilesDir(), "scans");
      Log.e(TAG, "No writable library at startup", failure);
    }
    preferences.edit().putString("SCANNER_LIBRARY_PATH", storageRoot.getAbsolutePath())
            .putBoolean("SCANNER_LIBRARY_APP_STORAGE", appStorage).apply();
    try {
      File marker = new File(storageRoot, ".nomedia");
      if (!marker.exists()) marker.createNewFile();
    } catch (IOException failure) {
      Log.w(TAG, "Unable to create library media marker", failure);
    }
  }

  public static boolean usesAppStorage() { return appStorage; }

  private static synchronized void initializeCaptureWorkspace(Context context) {
    if (captureWorkspace != null) return;
    internalCaptureRoot = new File(context.getFilesDir(), "capture-dataset");
    scratchRoot = new File(context.getCacheDir(), "scanner-work");
    captureWorkspace = new CaptureWorkspace(internalCaptureRoot,
            new File(getPath(false), TEMP_DIRECTORY),
            "modern".equals(com.lvonasek.arcore3dscanner.BuildConfig.FLAVOR)
                || "quality".equals(com.lvonasek.arcore3dscanner.BuildConfig.FLAVOR));
    captureRoot = captureWorkspace.select();
  }

  /** Only call from the idle library/recovery flow, never during an active capture. */
  protected static synchronized void refreshCaptureWorkspace() {
    captureRoot = captureWorkspace.select();
  }

  public static boolean hasPendingCapture() {
    return CaptureWorkspace.hasCaptureData(getTempPath());
  }

  public static boolean usesInternalCapture() {
    return internalCaptureRoot != null && internalCaptureRoot.equals(captureRoot);
  }

  /** Re-creatable editor/share scratch is separate from recoverable capture data. */
  public static File getScratchPath() {
    if (scratchRoot == null) throw new IllegalStateException("Storage has not been initialized");
    scratchRoot.mkdirs();
    return scratchRoot;
  }

  public float convertDpToPx(float dp) {
    return dp * ((float) getResources().getDisplayMetrics().densityDpi / DisplayMetrics.DENSITY_DEFAULT);
  }

  public float convertPxToDp(float px) {
    return px / ((float) getResources().getDisplayMetrics().densityDpi / DisplayMetrics.DENSITY_DEFAULT);
  }

  public static void deleteOnBackground(File file) {
    synchronized (toDelete) {

      //add file into queue
      boolean start = toDelete.isEmpty();
      toDelete.add(file);

      //start background thread
      if (start) {
        new Thread(() -> {
          while (true) {

            //get next file
            File f;
            synchronized (toDelete) {
              if (toDelete.isEmpty()) {
                return;
              }
              f = toDelete.remove(0);
            }

            //delete file
            IO.deleteRecursive(f);
          }
        }).start();
      }
    }
  }

  public static void deleteRecursive(File file) {
    for (int i = 0; i < 50; i++) {
      File finalFile = new File(file.getAbsolutePath() + DELETE_POSTFIX + i);
      if (file.renameTo(finalFile)) {
        deleteOnBackground(finalFile);
        return;
      }
    }
    IO.deleteRecursive(file);
  }

  public int getNavigationBarHeight() {
    Resources resources = getResources();
    int resourceId = resources.getIdentifier("navigation_bar_height", "dimen", "android");
    if (resourceId > 0) {
      return resources.getDimensionPixelSize(resourceId);
    }
    return 0;
  }

  public int getStatusBarHeight() {
    int result = 0;
    int resourceId = getResources().getIdentifier("status_bar_height", "dimen", "android");
    if (resourceId > 0) {
      result = getResources().getDimensionPixelSize(resourceId);
    }
    return result;
  }

  public abstract int getNavigationBarColor();

  public abstract int getStatusBarColor();

  public static int getBackend(Activity context)
  {
    return Compatibility.shouldUseHuawei(context) ? 1 : 0;
  }

  public static float getResolution(Context context)
  {
    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(context);
    String value = pref.getString(context.getString(com.lvonasek.arcore3dscanner.R.string.pref_resolution), "0.04");
    if (value.compareTo("0") == 0) value = "0.04";
    return Float.parseFloat(value);
  }

  public static boolean isCameraFeedOn(Context context) {
    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(context);
    return pref.getBoolean(context.getString(com.lvonasek.arcore3dscanner.R.string.pref_camera), false);
  }

  public static boolean isFaceModeOn(Context context)
  {
    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(context);
    return pref.getString(context.getString(com.lvonasek.arcore3dscanner.R.string.pref_mode), "realtime").compareTo("face") == 0;
  }

  public static boolean isProVersion(Context context) {
    return true;
  }

  public static boolean isPostProcessLaterOn(Context context) {
    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(context);
    boolean later = pref.getBoolean(context.getString(com.lvonasek.arcore3dscanner.R.string.pref_later),
            "modern".equals(com.lvonasek.arcore3dscanner.BuildConfig.FLAVOR));
    String mode = pref.getString(context.getString(com.lvonasek.arcore3dscanner.R.string.pref_mode), "realtime");
    return later || (mode.compareTo("dataset") == 0);
  }

  public static boolean isTofOn(Activity activity)
  {
    boolean supported = isTofSupported(activity);
    String key = activity.getString(com.lvonasek.arcore3dscanner.R.string.pref_depth);
    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(activity);
    return pref.getBoolean(key, supported) && supported;
  }

  public static boolean isTofSupported(Activity activity)
  {
    return getBackend(activity) == 0
            ? Compatibility.isGoogleToFSupported(activity)
            : Compatibility.isHuaweiToFSupported(activity);
  }

  public static ArrayList<String> listFiles(File file) {
    ArrayList<String> output = new ArrayList<>();
    File[] files = file.listFiles();
    if (files != null) {
      for (File f : files) {
        if (!f.getAbsolutePath().contains(DELETE_POSTFIX)) {
          output.add(f.getAbsolutePath());
        } else {
          deleteOnBackground(f);
        }
      }
    }
    return output;
  }

  public static void openURL(Activity context, String url) {

    //get apps capable of opening URL
    Intent intent = new Intent(Intent.ACTION_VIEW);
    intent.setData(Uri.parse(url));
    List<Intent> targetedShareIntents = new ArrayList<>();
    for (ResolveInfo info : context.getPackageManager().queryIntentActivities(intent, 0)) {
      Intent targetedShare = new Intent(Intent.ACTION_VIEW);
      targetedShare.setData(Uri.parse(url));
      if (!info.activityInfo.packageName.equalsIgnoreCase(context.getPackageName())) {
        targetedShare.setPackage(info.activityInfo.packageName);
        targetedShareIntents.add(targetedShare);
      }
    }

    //show chooser
    if (!targetedShareIntents.isEmpty()) {
      Intent chooserIntent = Intent.createChooser(targetedShareIntents.remove(0), url);
      chooserIntent.putExtra(Intent.EXTRA_INITIAL_INTENTS, targetedShareIntents.toArray(new Parcelable[0]));
      context.startActivity(chooserIntent);
    }
  }

  public static void setOrientation(boolean portrait, Activity activity) {
    int value = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;
    if (!portrait)
      value = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
    activity.setRequestedOrientation(value);
  }

  @Override
  protected void onPause() {
    mCompass.onPause();
    super.onPause();
  }

  @Override
  protected void onResume() {
    super.onResume();
    setWindow(getStatusBarColor(), getNavigationBarColor());
    setOrientation(true, this);

    mCompass = new Compass(this);
    mCompass.onResume();
  }

  public void setWindow(int statusBarColor, int navigationBarColor) {
    int lFlags = getWindow().getDecorView().getSystemUiVisibility();
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    getWindow().setStatusBarColor(statusBarColor);
    getWindow().setNavigationBarColor(navigationBarColor);
    if (Color.red(navigationBarColor) > 128)
      getWindow().getDecorView().setSystemUiVisibility(lFlags | View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
    else
      getWindow().getDecorView().setSystemUiVisibility(lFlags & ~View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
  }

  @Override
  public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
    if (requestCode == PERMISSIONS_CODE) {
      for (int r : grantResults) {
        if (r != PackageManager.PERMISSION_GRANTED) {
          if (onPermissionFail != null) {
            onPermissionFail.run();
          }
          onPermissionFail = null;
          onPermissionSuccess = null;
          return;
        }
      }
      if (onPermissionSuccess != null) {
        onPermissionSuccess.run();
      }
      onPermissionSuccess = null;
      onPermissionFail = null;
    }
    else
    {
      super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }
  }

  protected void askForPermissions(String[] permissions) {
    boolean ok = true;
    for (String s : permissions)
      if (checkSelfPermission(s) != PackageManager.PERMISSION_GRANTED)
        ok = false;

    if (!ok)
      requestPermissions(permissions, PERMISSIONS_CODE);
    else
      onRequestPermissionsResult(PERMISSIONS_CODE, null, new int[]{PackageManager.PERMISSION_GRANTED});
  }

  public static File getModel(File folder) {
    return ModelLookup.find(folder);
  }

  public static String getPath(boolean migrate) {
    String olddir = Environment.getExternalStorageDirectory().getPath() + OLD_MODEL_DIRECTORY;
    String newdir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS).getPath() + OLD_MODEL_DIRECTORY;
    File chosen = storageRoot;
    if (chosen == null) throw new IllegalStateException("Scan library has not been initialized");
    String dir = chosen.getAbsolutePath() + File.separator;

    if (migrate && !appStorage) {
      synchronized (migrationActive) {
        migrationActive.set(true);
      }
      migrate(olddir, dir);
      migrate(newdir, dir);
      synchronized (migrationActive) {
        migrationActive.set(false);
        if (restartApp.get()) {
          System.exit(0);
        }
      }
    }
    return dir;
  }

  public static File getTempPath() {
    File dir = captureRoot;
    if (dir == null) throw new IllegalStateException("Capture storage has not been initialized");
    if (dir.mkdir())
      Log.d(TAG, "Directory " + dir + " created");
    return dir;
  }

  public static boolean hasFilesToMigrate(Context context) {
    // Moving legacy public scans into app-owned storage would change their
    // uninstall lifetime; leave them untouched and let the user import copies.
    if (appStorage) return false;
    String key = "MIGRATION_DONE";
    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(context);
    if (pref.getBoolean(key, false)) {
      return false;
    }
    SharedPreferences.Editor e = pref.edit();
    e.putBoolean(key, true);
    e.commit();

    String olddir = Environment.getExternalStorageDirectory().getPath() + OLD_MODEL_DIRECTORY;
    String newdir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS).getPath() + OLD_MODEL_DIRECTORY;
    if (new File(olddir).exists()) {
      File[] files = new File(olddir).listFiles();
      if ((files != null) && (files.length > 0)) {
        return true;
      }
    }
    if (new File(newdir).exists()) {
      File[] files = new File(newdir).listFiles();
      if ((files != null) && (files.length > 0)) {
        return true;
      }
    }
    return false;
  }

  private static void migrate(String olddir, String newdir) {
    Log.d(TAG, "Migrating " + olddir + " into " + newdir);
    if (new File(olddir).exists()) {
      boolean ok = true;
      File[] files = new File(olddir).listFiles();
      if (files != null) {
        for (File file : files) {
          if (file.renameTo(new File(newdir, file.getName()))) {
            Log.d(TAG, file.getName() + " migrated");
          } else {
            Log.d(TAG, "Unable to migrate " + file.getName());
            ok = false;
          }
        }
      }
      if (ok) {
        if (new File(olddir).delete()) {
          Log.d(TAG, "Directory " + olddir + " deleted");
        }
      }
    }
  }
}
