package com.lvonasek.arcore3dscanner.sharing;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.widget.ProgressBar;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.core.content.FileProvider;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.lvonasek.arcore3dscanner.BuildConfig;
import com.lvonasek.arcore3dscanner.R;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;

/** Viewer/browser sharing boundary. Invoke from the UI thread with a captured selection. */
public final class ModelSharing {
  private static final AtomicBoolean BUSY = new AtomicBoolean();

  private ModelSharing() {}

  public static boolean share(Activity activity, File selected) {
    return share(activity, selected, () -> {});
  }

  /** onLaunched runs synchronously after a successful launch, before Activity.onPause. */
  public static boolean share(Activity activity, File selected, Runnable onLaunched) {
    if (Looper.myLooper() != Looper.getMainLooper())
      throw new IllegalStateException("ModelSharing must be called on the UI thread.");
    if (activity.isFinishing() || activity.isDestroyed()) return false;
    // Capture both paths now; workers must never consult mutable browser/viewer state.
    final File source = selected.getAbsoluteFile();
    final File cache = activity.getCacheDir();
    final AlertDialog progress = new MaterialAlertDialogBuilder(activity)
        .setTitle(R.string.share_model_title)
        .setMessage(R.string.share_model_preparing)
        .setView(new ProgressBar(activity))
        .setCancelable(false)
        .create();
    if (!BUSY.compareAndSet(false, true)) {
      Toast.makeText(activity, R.string.share_model_busy, Toast.LENGTH_LONG).show();
      return false;
    }
    try {
      progress.show();
      new Thread(() -> {
        ModelPackage.Result result = null;
        Exception failure = null;
        try { result = ModelPackage.prepare(source, cache); }
        catch (Exception e) { failure = e; }
        final ModelPackage.Result prepared = result;
        final Exception error = failure;
        new Handler(Looper.getMainLooper()).post(() -> {
          try {
            if (!activity.isDestroyed()) progress.dismiss();
            if (activity.isFinishing() || activity.isDestroyed()) return;
            if (error != null) throw error;
            activity.startActivity(createChooser(activity, prepared));
            onLaunched.run();
          } catch (Exception e) {
            if (!activity.isFinishing() && !activity.isDestroyed()) showError(activity, e);
          } finally {
            BUSY.set(false);
          }
        });
      }, "model-sharing").start();
      return true;
    } catch (RuntimeException e) {
      progress.dismiss();
      BUSY.set(false);
      showError(activity, e);
      return false;
    }
  }

  /** Actual ACTION_SEND and chooser both carry ClipData and explicit read permission. */
  public static Intent createChooser(Context context, ModelPackage.Result result) {
    return createChooser(context, result.file, result.mimeType, R.string.share_model_title);
  }

  public static Intent createChooser(Context context, File file, String mimeType, int title) {
    Uri uri = FileProvider.getUriForFile(context, BuildConfig.APPLICATION_ID + ".provider", file);
    Intent send = createSendIntent(uri, mimeType, file.getName());
    Intent chooser = Intent.createChooser(send, context.getString(title));
    chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
    chooser.setClipData(send.getClipData());
    return chooser;
  }

  public static Intent createSendIntent(Uri uri, String mimeType, String name) {
    if (!"content".equals(uri.getScheme())) throw new IllegalArgumentException("Model sharing requires a content URI.");
    Intent send = new Intent(Intent.ACTION_SEND);
    send.setType(mimeType);
    send.putExtra(Intent.EXTRA_STREAM, uri);
    send.setClipData(ClipData.newRawUri(name, uri));
    send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
    return send;
  }

  private static void showError(Activity activity, Exception error) {
    String message = error instanceof ModelPackage.DatasetException
        ? activity.getString(R.string.share_model_dataset_export)
        : activity.getString(R.string.share_model_failed, error.getMessage() == null
            ? error.getClass().getSimpleName() : error.getMessage());
    new MaterialAlertDialogBuilder(activity)
        .setTitle(R.string.share_model_title)
        .setMessage(message)
        .setPositiveButton(android.R.string.ok, null)
        .show();
  }
}
