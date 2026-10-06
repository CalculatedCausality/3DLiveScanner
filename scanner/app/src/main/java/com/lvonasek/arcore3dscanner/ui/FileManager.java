package com.lvonasek.arcore3dscanner.ui;

import android.Manifest;
import androidx.appcompat.app.AlertDialog;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import android.app.Dialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Bundle;
import android.preference.PreferenceManager;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.GridView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.widget.Toast;

import com.google.ar.core.ArCoreApk;
import com.lvonasek.arcore3dscanner.R;
import com.lvonasek.arcore3dscanner.main.Exporter;
import com.lvonasek.arcore3dscanner.main.Main;
import com.lvonasek.arcore3dscanner.main.JNI;
import com.lvonasek.utils.Compatibility;
import com.lvonasek.utils.IO;

import java.io.File;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Locale;
import java.util.Scanner;

public class FileManager extends AbstractActivity implements View.OnClickListener {
  private FileAdapter mAdapter;
  private GridView mList;
  private Button mAdd;
  private Button mCancel;
  private ProgressBar mProgress;
  private TextView mText;
  private View mHeader;
  private LinearLayout mOptions;
  private TextView mName;
  private View mPosition;
  private View mRename;
  private View mShare;
  private int mInterruptedScan;
  private boolean mRecoveryDialogVisible;
  private volatile boolean mRecoveryCheckPending;
  private volatile int mPollingGeneration;
  private boolean mExportInProgress;
  private static boolean allowedToAskForPermissions = true;

  @Override
  protected void onCreate(Bundle savedInstanceState)
  {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_files);
    if (usesAppStorage()) {
      ((TextView) findViewById(R.id.library_hint)).setText(
              getString(R.string.library_hint) + "\n" + getString(R.string.storage_app_library));
    }

    findViewById(R.id.settings).setOnClickListener(this);
    findViewById(R.id.close_selection).setOnClickListener(v -> mAdapter.update());

    mName = findViewById(R.id.name);
    mRename = findViewById(R.id.rename);
    mPosition = findViewById(R.id.position);
    mShare = findViewById(R.id.share);
    mHeader = findViewById(R.id.header);
    mOptions = findViewById(R.id.options);
    mPosition.setOnClickListener(this);
    mRename.setOnClickListener(this);
    mShare.setOnClickListener(this);
    findViewById(R.id.delete).setOnClickListener(this);

    mAdd = findViewById(R.id.add_button);
    mCancel = findViewById(R.id.service_cancel);
    mList = findViewById(R.id.list);
    mText = findViewById(R.id.info_text);
    mProgress = findViewById(R.id.progressBar);
    mAdd.setOnClickListener(this);
    mCancel.setOnClickListener(this);

    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(this);
    int columns = pref.getInt(getString(R.string.pref_layout), 2);

    mAdapter = new FileAdapter(this, columns);
    mList.setOnTouchListener((view, event) -> {
      mAdapter.forwardTouch(event);
      return false;
    });
  }

  @Override
  public void onBackPressed()
  {
    if (mProgress.getVisibility() == View.VISIBLE) {
      System.exit(0);
    } else if (mAdapter.getSelected() != null) {
      mAdapter.update();
    } else if (mAdapter.hasParent()) {
      mAdapter.toParent();
    } else {
      moveTaskToBack(true);
    }
  }

  @Override
  public int getNavigationBarColor() {
        return getColor(R.color.scanner_background);
    }

  @Override
  public int getStatusBarColor() {
    return getColor(R.color.scanner_background);
  }

  @Override
  protected void onResume()
  {
    super.onResume();
    final int pollingGeneration = ++mPollingGeneration;
    mAdd.setVisibility(View.VISIBLE);
    mCancel.setVisibility(View.GONE);
    mCancel.setText(R.string.action_cancel);
    mCancel.setOnClickListener(this);
    mProgress.setVisibility(View.GONE);

    int service = Service.getRunning(this);
    if ((service > Service.SERVICE_NOT_RUNNING) && !Service.isActive()) {
      Service.clearAbandonedState(this);
      service = Service.SERVICE_NOT_RUNNING;
    }
    if (service > Service.SERVICE_NOT_RUNNING) {
      mAdd.setVisibility(View.GONE);
      mCancel.setVisibility(View.VISIBLE);
      mList.setVisibility(View.GONE);
      setMessageVisible(true);
      mText.setText("");
      new Thread(() -> {
        while (pollingGeneration == mPollingGeneration) {
          try
          {
            Thread.sleep(1000);
          } catch (Exception e)
          {
            e.printStackTrace();
          }
          FileManager.this.runOnUiThread(() -> {
            if (pollingGeneration != mPollingGeneration) return;
            if (Service.getMessage() == null)
              mText.setText(getString(R.string.failed));
            else
              mText.setText(getString(R.string.working) + "\n\n" + Service.getMessage());
          });
        }
      }).start();
    } else if (service < Service.SERVICE_NOT_RUNNING)
    {
      service = Math.abs(service);
      mAdd.setVisibility(View.GONE);
      if (service != Service.SERVICE_SAVE) {
        mCancel.setVisibility(View.VISIBLE);
        mList.setVisibility(View.GONE);
        setMessageVisible(true);
      }
      boolean paused = service == Service.SERVICE_SAVE;
      int text = paused ? R.string.paused : R.string.finished;
      mText.setText(getString(text) + "\n" + getString(R.string.turn_off));
      if (service == Service.SERVICE_SAVE) {
        showProgress();
        startActivity(new Intent(this, Main.class));
      } else if ((service == Service.SERVICE_POSTPROCESS) || (service == Service.SERVICE_PHOTOGRAMMETRY)) {
        finishScanning();
      }
    } else
      setupPermissions();
  }

  public void refreshUI()
  {
    refreshCaptureWorkspace();
    String hint = getString(R.string.library_hint);
    if (usesAppStorage()) hint += "\n" + getString(R.string.storage_app_library);
    if (usesInternalCapture()) hint += "\n" + getString(R.string.storage_capture_internal);
    ((TextView) findViewById(R.id.library_hint)).setText(hint);
    // Privacy information is available on demand in Settings. Library refresh,
    // imports and app updates should not repeatedly display a startup modal.

    long time = System.currentTimeMillis();
    boolean migrate = hasFilesToMigrate(this);
    if (migrate) {
      Log.d(TAG, "Some files has to be migrated");
    }
    mCancel.setVisibility(View.GONE);
    findViewById(R.id.checkbox).setVisibility(View.GONE);
    mCancel.setEnabled(true);
    mAdd.setVisibility(View.GONE);
    mList.setVisibility(View.VISIBLE);
    mText.setOnClickListener(null);
    mText.setText(migrate ? R.string.migrating_data : R.string.wait);
    setMessageVisible(mAdapter.isEmpty());
    mRecoveryCheckPending = true;
    new Thread(() -> {

      //update file structure
      Exporter.makeStructure(getPath(migrate));
      mInterruptedScan = getInterruptedScanState();
      mRecoveryCheckPending = false;

      //get list of files
      runOnUiThread(() -> {
        mAdapter.update();
        Log.d(TAG, "Listing files took " + (System.currentTimeMillis() - time) + "ms");

        mText.setText(R.string.empty_scans_guidance);
        setMessageVisible(mAdapter.getCount() == 0);
        mList.setAdapter(mAdapter);
        mAdd.setVisibility(View.VISIBLE);
        mProgress.setVisibility(View.GONE);

        if (mAdapter.getCount() > 0) {
          mList.setSelection(0);
        }
        if (mInterruptedScan != 0) showInterruptedScanDialog();
      });
    }).start();
  }

  protected void setupPermissions() {
    String[] permissions = {
            Manifest.permission.CAMERA,
            Manifest.permission.INTERNET
    };

    boolean ok = true;
    for (String s : permissions)
      if (checkSelfPermission(s) != PackageManager.PERMISSION_GRANTED)
        ok = false;

    if (!allowedToAskForPermissions && !ok) {
      mAdd.setVisibility(View.GONE);
      mCancel.setVisibility(View.VISIBLE);
      mList.setVisibility(View.GONE);
      mCancel.setText(android.R.string.ok);
      mCancel.setOnClickListener(view -> {
        allowedToAskForPermissions = true;
        setupPermissions();
      });
      mText.setText(R.string.permissions_required);
      setMessageVisible(true);
      return;
    } else {
      mAdd.setVisibility(View.VISIBLE);
      mList.setVisibility(View.VISIBLE);
      mCancel.setText(android.R.string.cancel);
      mCancel.setOnClickListener(this);
      mCancel.setVisibility(View.GONE);
      allowedToAskForPermissions = false;
    }

    try {
      boolean arcore = Compatibility.isPlayStoreSupported(this);
      boolean arengine = Compatibility.shouldUseHuawei(this);
      if ((!arengine || arcore) && Compatibility.isARSupported(this))
        if (ArCoreApk.getInstance().requestInstall(this, true) != ArCoreApk.InstallStatus.INSTALLED)
          return;
    } catch (Exception e) {
      e.printStackTrace();
    }

    long timestamp = System.currentTimeMillis();
    onPermissionFail = () -> {
      if (System.currentTimeMillis() - timestamp < 100) {
        Intent intent = new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
        Uri uri = Uri.fromParts("package", getPackageName(), null);
        intent.setData(uri);
        startActivity(intent);
      }
    };
    onPermissionSuccess = () -> {
      if (Initializator.hasFileIntent()) {
        showProgress();

        new Thread(() -> {

          File staging = null;
          try {
            File library = new File(getPath(false));
            staging = IO.createStagingDirectory(library);
            if (!IO.unzip(staging.getAbsolutePath(), Initializator.getFile(FileManager.this))) {
              throw new IOException("Archive import failed");
            }
            File model = ModelLookup.find(staging);
            String extension;
            if (model != null && model.length() > 0) {
              extension = ModelLookup.extension(model);
              if (Exporter.EXT_OBJ.equals(extension)) {
                for (String resource : Exporter.getObjResources(model)) {
                  if (!IO.resolveContainedFile(staging, resource).isFile()) {
                    throw new IOException("Missing imported resource: " + resource);
                  }
                }
              }
            } else if (new File(staging, "state.txt").isFile()
                    && JNI.isDatasetValid(staging.getAbsolutePath().getBytes())) {
              extension = Exporter.EXT_DATASET;
              model = null;
            } else {
              throw new IOException("Archive contains no supported model or valid dataset");
            }
            int index = 1;
            File destination;
            do {
              destination = new File(library, "Import_" + index++ + extension);
            } while (destination.exists());
            if (!staging.renameTo(destination)) throw new IOException("Unable to publish imported scan");
            final File imported = model == null ? null : new File(destination, model.getName());
            runOnUiThread(() -> {
              if (isFinishing() || isDestroyed()) return;
              if (imported != null) {
                Intent intent = new Intent(FileManager.this, Main.class);
                intent.putExtra(AbstractActivity.FILE_KEY, imported.getAbsolutePath());
                startActivity(intent);
              } else {
                Toast.makeText(this, R.string.storage_imported, Toast.LENGTH_LONG).show();
                refreshUI();
              }
            });
          } catch (Exception failure) {
            Log.e(TAG, "Unable to import scan", failure);
            runOnUiThread(() -> {
              if (isFinishing() || isDestroyed()) return;
              Toast.makeText(this, R.string.storage_import_failed, Toast.LENGTH_LONG).show();
              refreshUI();
            });
          } finally {
            if (staging != null) IO.deleteRecursive(staging);
          }
        }).start();
      } else {
        refreshUI();
      }
    };
    askForPermissions(permissions);
  }

  public void showProgress()
  {
    mAdd.setVisibility(View.GONE);
    mProgress.setVisibility(View.VISIBLE);
  }

  @Override
  public void onClick(View v) {
    int id = v.getId();

    if (id == R.id.delete) {
      mAdapter.deleteModel();
    } else if (id == R.id.position) {
      mAdapter.showPosition();
    } else if (id == R.id.rename) {
      mAdapter.rename();
    } else if (id == R.id.share) {
      mAdapter.shareModel();
    } else if (id == R.id.add_button) {
      SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(this);
      if (pref.getBoolean(getString(R.string.pref_gps), false)) {
        String[] permissions = {
                Manifest.permission.ACCESS_COARSE_LOCATION,
                Manifest.permission.ACCESS_FINE_LOCATION
        };
        onPermissionSuccess = this::startScanning;
        askForPermissions(permissions);
      } else {
        startScanning();
      }
    } else if (id == R.id.service_cancel) {
      Service.reset(this);
      System.exit(0);
    } else if (id == R.id.settings) {
      startActivity(new Intent(this, Settings.class));
    }
  }


  private void startScanning()
  {
    if (mRecoveryCheckPending) return;
    refreshCaptureWorkspace();
    if (!hasPendingCapture()) mInterruptedScan = 0;
    else if (mInterruptedScan == 0) {
      // Full native dataset validation remains on refreshUI's background worker.
      refreshUI();
      return;
    }
    if (mInterruptedScan != 0) {
      showInterruptedScanDialog();
      return;
    }
    ArrayList<Drawable> icons = new ArrayList<>();
    ArrayList<String> values = new ArrayList<>();
    if (Compatibility.isARSupported(this)) {
      boolean captureFirst="modern".equals(com.lvonasek.arcore3dscanner.BuildConfig.FLAVOR);
      if (captureFirst) {
        icons.add(getDrawable(R.drawable.ic_type_dataset));
        values.add(getString(R.string.mode_dataset));
      }
      icons.add(getDrawable(R.drawable.ic_type_face));
      values.add(getString(R.string.mode_face));
      icons.add(getDrawable(R.drawable.ic_type_scan));
      values.add(getString(R.string.mode_realtime));
      if (!captureFirst) {
        icons.add(getDrawable(R.drawable.ic_type_dataset));
        values.add(getString(R.string.mode_dataset));
      }
    }

    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(FileManager.this);
    Dialog dialog = CommonDialogs.showScanChoices(this, values, icons, 0);
    GridView list = dialog.findViewById(R.id.list);
    list.setOnItemClickListener((adapterView, view, index, l) -> {
      dialog.dismiss();
      showProgress();

      String mode = values.get(index);
      SharedPreferences.Editor e = pref.edit();
      e.putBoolean(getString(R.string.pref_later), mode.equals(getString(R.string.mode_dataset)));
      e.putString(getString(R.string.pref_mode), mode.equals(getString(R.string.mode_face)) ? "face" : "realtime");
      e.commit();

      startActivity(new Intent(FileManager.this, Main.class));
    });
  }

  @Override
  protected void onPause()
  {
    ++mPollingGeneration;
    super.onPause();
  }

  @Override
  protected void onDestroy() {
    if (mAdapter != null) mAdapter.close();
    super.onDestroy();
  }

  private int getInterruptedScanState() {
    File temp = getTempPath();
    File state = new File(temp, "state.txt");
    if (!state.isFile()) return hasPendingCapture() ? 2 : 0;

    int count;
    int width;
    int height;
    try (Scanner scanner = new Scanner(state).useLocale(Locale.US)) {
      count = scanner.nextInt();
      width = scanner.nextInt();
      height = scanner.nextInt();
      double cx = scanner.nextDouble();
      double cy = scanner.nextDouble();
      double fx = scanner.nextDouble();
      double fy = scanner.nextDouble();
      if ((count <= 0) || (count > 100000) || (width <= 0) || (height <= 0)
              || (width > 8192) || (height > 8192)
              || !Double.isFinite(cx) || !Double.isFinite(cy)
              || !Double.isFinite(fx) || !Double.isFinite(fy)
              || (fx <= 0) || (fy <= 0)) return 2;
    } catch (Exception e) {
      return 2;
    }

    return JNI.isDatasetValid(temp.getAbsolutePath().getBytes()) ? 1 : 2;
  }

  private void showInterruptedScanDialog() {
    if (mRecoveryDialogVisible || isFinishing()) return;
    mRecoveryDialogVisible = true;
    AlertDialog.Builder builder = new MaterialAlertDialogBuilder(this)
            .setTitle(R.string.recovery_title)
            .setMessage(mInterruptedScan == 1 ? R.string.recovery_message : R.string.recovery_corrupt)
            .setNegativeButton(R.string.delete, (dialog, which) -> {
              File source = getTempPath();
              showProgress();
              new Thread(() -> {
                IO.deleteRecursive(source);
                runOnUiThread(this::refreshUI);
              }, "discard-capture").start();
            })
            .setNeutralButton(R.string.recovery_keep, null)
            .setOnDismissListener(dialog -> mRecoveryDialogVisible = false);
    if (mInterruptedScan == 1) {
      builder.setPositiveButton(R.string.recovery_finalize, (dialog, which) -> recoverInterruptedDataset());
    }
    AlertDialog dialog = builder.create();
    dialog.setCanceledOnTouchOutside(false);
    dialog.show();
  }

  private void recoverInterruptedDataset() {
    showProgress();
    new Thread(() -> {
      File source = getTempPath();
      String base = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
      File destination = new File(getPath(false), base + Exporter.EXT_DATASET);
      int suffix = 1;
      while (destination.exists()) {
        destination = new File(getPath(false), base + "_" + suffix++ + Exporter.EXT_DATASET);
      }
      boolean published = false;
      try {
        if (getInterruptedScanState() != 1) throw new IOException("Invalid interrupted capture");
        IO.publishDirectoryChecked(source, destination);
        published = true;
        IO.deleteRecursive(source);
      } catch (Exception failure) {
        Log.e(TAG, "Unable to publish recovered dataset; source retained", failure);
      }
      final boolean recovered = published;
      runOnUiThread(() -> {
        if (recovered) {
          mInterruptedScan = 0;
          refreshUI();
        } else {
          mProgress.setVisibility(View.GONE);
          mAdd.setVisibility(View.VISIBLE);
          Toast.makeText(this, R.string.recovery_failed, Toast.LENGTH_LONG).show();
          showInterruptedScanDialog();
        }
      });
    }, "scan-recovery").start();
  }

  private void finishScanning()
  {
    if (mExportInProgress) return;
    mExportInProgress = true;
    mCancel.setVisibility(View.GONE);
    showProgress();
    final String filename = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
    final String source = Service.getLink(this);

    new Thread(() -> {
      try {
        if (source == null || source.isEmpty()) throw new IOException("No completed model to save");
        File file = new File(source);
        String extension = ModelLookup.extension(file);
        if (extension == null) throw new IOException("Unsupported completed model");
        String availableName = filename;
        int suffix = 1;
        while (new File(getPath(false), availableName + extension).exists()) {
          availableName = filename + "_" + suffix++;
        }
        File file2save = Exporter.export(file, availableName);
        Service.reset(FileManager.this);

        // Only release the scanner's temporary dataset, never an arbitrary source folder.
        try {
          File workspace = getTempPath();
          if (!isPostProcessLaterOn(FileManager.this)
                  && file.getAbsoluteFile().getParentFile().getCanonicalFile().equals(workspace.getCanonicalFile())) {
            IO.deleteRecursive(workspace);
          }
        } catch (Exception cleanupFailure) {
          // Publication already succeeded; a cleanup failure must not become a failed save.
          Log.w(TAG, "Saved model but retained temporary data", cleanupFailure);
        }
        runOnUiThread(() -> {
          mExportInProgress = false;
          if (isFinishing() || isDestroyed()) return;
          Toast.makeText(this, getString(R.string.data_saved) + " " + file2save.getName(), Toast.LENGTH_LONG).show();
          Intent intent = new Intent(FileManager.this, Main.class);
          intent.putExtra(FILE_KEY, file2save.getAbsolutePath());
          startActivity(intent);
        });
      } catch (Exception failure) {
        Log.e(TAG, "Unable to publish completed scan; retaining source", failure);
        runOnUiThread(() -> {
          mExportInProgress = false;
          if (isFinishing() || isDestroyed()) return;
          mProgress.setVisibility(View.GONE);
          mAdd.setVisibility(View.GONE);
          mList.setVisibility(View.GONE);
          setMessageVisible(true);
          mText.setText(R.string.storage_export_failed);
          mCancel.setText(R.string.storage_retry);
          mCancel.setOnClickListener(view -> finishScanning());
          mCancel.setVisibility(View.VISIBLE);
        });
      }
    }).start();
  }

  private void setMessageVisible(boolean visible) {
    mText.setVisibility(visible ? View.VISIBLE : View.GONE);
    findViewById(R.id.library_message_container).setVisibility(visible ? View.VISIBLE : View.GONE);
  }

  public void setColumns(int count) {
    mList.setNumColumns(count);

    SharedPreferences.Editor e = PreferenceManager.getDefaultSharedPreferences(this).edit();
    e.putInt(getString(R.string.pref_layout), count);
    e.commit();
  }

  public void setOptions(int size) {
    boolean on = size > 0;
    mHeader.setVisibility(on ? View.INVISIBLE : View.VISIBLE);
    mOptions.setVisibility(on ? View.VISIBLE : View.GONE);
    RelativeLayout.LayoutParams listParams = (RelativeLayout.LayoutParams) mList.getLayoutParams();
    listParams.addRule(RelativeLayout.BELOW, on ? R.id.options : R.id.header);
    mList.setLayoutParams(listParams);

    if (on) {
      mName.setText(mAdapter.getSelected());
    }

    boolean more = size > 1;
    boolean ext = mAdapter.hasExtension();
    mPosition.setVisibility(!more && mAdapter.hasPosition() ? View.VISIBLE : View.GONE);
    mRename.setVisibility(!more ? View.VISIBLE : View.GONE);
    mShare.setVisibility(ext && !more ? View.VISIBLE : View.GONE);

    int background = getColor(R.color.scanner_surface_high);
    setWindow(on ? background : getStatusBarColor(), getNavigationBarColor());
  }
}
