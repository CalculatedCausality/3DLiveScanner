package com.lvonasek.arcore3dscanner.ui;

import com.google.android.material.card.MaterialCardView;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import android.app.Dialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.preference.PreferenceManager;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.GridView;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import com.lvonasek.arcore3dscanner.R;
import com.lvonasek.arcore3dscanner.main.Exporter;
import com.lvonasek.arcore3dscanner.main.Main;
import com.lvonasek.arcore3dscanner.sharing.ModelSharing;
import com.lvonasek.utils.GestureDetector;

import java.io.File;
import java.io.FileInputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Locale;
import java.util.Scanner;

class FileAdapter extends BaseAdapter
{
  private final FileManager mContext;
  private File mPath;
  private final ArrayList<Integer> mSelected = new ArrayList<>();
  private final GestureDetector mGesture;
  private float mColumns;
  private boolean mArchiveSharing;

  private final ThumbnailLoader mThumbnails = new ThumbnailLoader();
  private final ArrayList<String> mItems = new ArrayList<>();

  FileAdapter(FileManager context, int columns)
  {
    mContext = context;
    mColumns = columns;
    mPath = new File(AbstractActivity.getPath(false));

    mContext.setColumns(columns);
    mGesture = new GestureDetector(new GestureDetector.GestureListener() {
      @Override
      public boolean IsAcceptingRotation() {
        return false;
      }

      @Override
      public void OnDrag(float dx, float dy) {
      }

      @Override
      public void OnTwoFingerMove(float dx, float dy) {
      }

      @Override
      public void OnTwoFingerRotation(float angle) {
      }

      @Override
      public void OnPinchToZoom(float diff) {
        int before = (int)mColumns;
        mColumns = Math.max(2, Math.min(5, mColumns - diff * 0.75f));
        int after = (int)mColumns;

        if (before != after) {
          mContext.setColumns(after);
        }
      }
    }, mContext);
  }

  @Override
  public int getCount()
  {
    return mItems.size();
  }

  @Override
  public Object getItem(int i)
  {
    return mItems.get(i);
  }

  @Override
  public long getItemId(int i)
  {
    return i;
  }

  @Override
  public View getView(final int index, View view, ViewGroup viewGroup)
  {
    if (view == null) {
      view = LayoutInflater.from(mContext).inflate(R.layout.view_item, viewGroup, false);
    }
    if (getCount() <= index) {
      return view;
    }
    String key = (String)getItem(index);
    File file = new File(mPath, key);
    TextView name = view.findViewById(R.id.name);
    if (key.lastIndexOf('.') > 0) {
      name.setText(key.substring(0, key.lastIndexOf('.')));
    } else {
      name.setText(key);
    }

    //set icon
    boolean hasExtension = false;
    ImageView icon = view.findViewById(R.id.icon);
    icon.setImageDrawable(mContext.getDrawable(R.drawable.ic_folder));
    if (key.compareTo(mContext.getString(R.string.folder_up)) == 0) {
      icon.setImageDrawable(mContext.getDrawable(R.drawable.ic_folder_up));
    }
    for (String ext : Exporter.FILE_EXT) {
      if (key.endsWith(ext)) {
        icon.setImageDrawable(mContext.getDrawable(R.drawable.ic_model_icon));
        hasExtension = true;
        break;
      }
    }
    mThumbnails.bind(icon, key.endsWith(Exporter.EXT_DATASET) || key.endsWith(Exporter.EXT_OBJ)
            ? file : null);

    //set extension
    TextView extension = view.findViewById(R.id.extension);
    int type = !hasExtension ? R.string.model_type_folder
            : (key.endsWith(Exporter.EXT_DATASET) ? R.string.model_type_dataset
            : (key.endsWith(Exporter.EXT_PLY) ? R.string.model_type_points : R.string.model_type_mesh));
    extension.setText(type);
    view.setTag(mContext.getString(R.string.model_accessibility, name.getText(), extension.getText()));

    //set selection
    View selection = view.findViewById(R.id.selection);
    updateSelection(selection, mSelected.contains(index));

    //set open action
    boolean finalHasExtension = hasExtension;
    view.setOnClickListener(v -> {
      if (!mSelected.isEmpty()) {
        setSelected(selection, index);
        return;
      }

      if (!finalHasExtension) {
        if (hasParent() && (index == 0)) {
          toParent();
        } else {
          mThumbnails.clear();
          mPath = file;
          mContext.refreshUI();
        }
      } else if (key.endsWith(Exporter.EXT_DATASET)) {
        startPostprocess(file);
      } else {
        Intent intent = new Intent(mContext, Main.class);
        intent.putExtra(AbstractActivity.FILE_KEY, file.getAbsolutePath());
        mContext.showProgress();
        mContext.startActivity(intent);
      }
    });

    view.setOnLongClickListener(view1 -> {
      setSelected(selection, index);
      return true;
    });

    return view;
  }

  public boolean hasParent() {
    return !mPath.equals(new File(AbstractActivity.getPath(false)));
  }

  public void toParent() {
    mThumbnails.clear();
    mPath = mPath.getParentFile();
    mContext.refreshUI();
  }

  public void close() {
    mThumbnails.close();
  }

  private void startPostprocess(File file) {
    ArrayList<Drawable> icons = new ArrayList<>();
    ArrayList<String> values = new ArrayList<>();
    values.add(mContext.getString(R.string.export_model));
    values.add(mContext.getString(R.string.export_floorplan));
    values.add(mContext.getString(R.string.export_pointcloud));
    icons.add(mContext.getDrawable(R.drawable.ic_type_scan));
    icons.add(mContext.getDrawable(R.drawable.ic_type_floorplan));
    icons.add(mContext.getDrawable(R.drawable.ic_type_pointcloud));

    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(mContext);
    Dialog dialog = CommonDialogs.showScanChoices(mContext, values, icons, R.string.export);
    GridView list = dialog.findViewById(R.id.list);
    list.setOnItemClickListener((adapterView, view, index, l) -> {
      SharedPreferences.Editor e = pref.edit();
      e.putBoolean(mContext.getString(R.string.pref_later), true);
      String[] modes = {"realtime", "exp_floorplan", "exp_pointcloud"};
      e.putString(mContext.getString(R.string.pref_mode), modes[index]);
      e.commit();

      Intent intent = new Intent(mContext, Main.class);
      intent.putExtra(AbstractActivity.FILE_KEY, file.getAbsolutePath());
      dialog.dismiss();
      mContext.startActivity(intent);
    });
  }

  public void update() {
    mThumbnails.clear();
    mItems.clear();
    mSelected.clear();
    mContext.setOptions(mSelected.size());

    if (hasParent()) {
      mItems.add(mContext.getString(R.string.folder_up));
    }

    String[] files = mPath.list();
    if (files != null) {
      Arrays.sort(files);

      ArrayList<String> folders = new ArrayList<>();
      ArrayList<String> data = new ArrayList<>();
      for (String s : files) {
        if (s.contains(AbstractActivity.DELETE_POSTFIX)) {
          continue;
        }
        File f = new File(mPath, s);
        if (!f.isDirectory() || f.getAbsolutePath().equals(mContext.getTempPath().getAbsolutePath())) {
          continue;
        }
        if (Exporter.isFolder(s)) {
          folders.add(s);
        } else if (s.startsWith("20")) {
          data.add(0, s);
        } else {
          data.add(s);
        }
      }

      mItems.addAll(folders);
      mItems.addAll(data);
    }
    notifyDataSetChanged();
  }

  public boolean hasPosition() {
    if (mSelected.isEmpty()) {
      return false;
    }
    String key = (String)getItem(mSelected.get(0));
    File gpsFile = new File(new File(mPath, key), "position.txt");
    return gpsFile.exists();
  }

  public void showPosition() {
    String key = (String)getItem(mSelected.get(0));
    File gpsFile = new File(new File(mPath, key), "position.txt");
    try {
      String lon;
      String lat;
      try (Scanner sc = new Scanner(new FileInputStream(gpsFile))) {
        sc.useLocale(Locale.US);
        lon = sc.next();
        lat = sc.next();
      }

      Uri uri = Uri.parse("geo:" + lat + "," + lon);
      Intent intent = new Intent(android.content.Intent.ACTION_VIEW, uri);
      mContext.startActivity(intent);
    } catch (Exception e) {
      e.printStackTrace();
    }
  }

  public void deleteModel() {
    ArrayList<File> selected = new ArrayList<>();
    for (int index : mSelected) selected.add(new File(mPath, (String)getItem(index)));
    CommonDialogs.confirmDialog(mContext, R.string.delete, () -> {
      for (File file : selected) AbstractActivity.deleteRecursive(file);
      mContext.refreshUI();
    });
  }

  public void shareModel() {
    if (mSelected.isEmpty()) return;
    String key = (String)getItem(mSelected.get(0));
    final File selected = new File(mPath, key);
    if (key.endsWith(Exporter.EXT_DATASET)) {
      new MaterialAlertDialogBuilder(mContext)
          .setTitle(R.string.share_model_title)
          .setItems(new CharSequence[]{mContext.getString(R.string.share_dataset_export),
                  mContext.getString(R.string.share_dataset_archive)}, (dialog, which) -> {
            if (which == 0) startPostprocess(selected);
            else shareDatasetArchive(selected);
          }).show();
    } else {
      ModelSharing.share(mContext, selected);
    }
  }

  private void shareDatasetArchive(File selected) {
    if (mArchiveSharing) return;
    mArchiveSharing = true;
    mContext.showProgress();
    new Thread(() -> {
      String archive = null;
      Exception failure = null;
      try { archive = Exporter.compressModel(selected); }
      catch (Exception error) { failure = error; }
      final String prepared = archive;
      final Exception error = failure;
      mContext.runOnUiThread(() -> {
        mArchiveSharing = false;
        if (mContext.isFinishing() || mContext.isDestroyed()) return;
        mContext.refreshUI();
        try {
          if (error != null) throw error;
          mContext.startActivity(ModelSharing.createChooser(mContext, new File(prepared),
                  "application/zip", R.string.share_dataset_archive));
        } catch (Exception errorSharing) {
          Toast.makeText(mContext, mContext.getString(R.string.share_model_failed,
                  errorSharing.getMessage()), Toast.LENGTH_LONG).show();
        }
      });
    }, "share-dataset").start();
  }

  public void rename() {
    String key = (String)getItem(mSelected.get(0));
    new RenameDialog(mContext, mPath.getAbsolutePath(), key);
  }

  public void setSelected(View selection, int index) {
    if (hasParent() && (index == 0)) {
      return;
    }
    if (mSelected.contains(index)) {
      mSelected.remove((Integer) index);
    } else {
      mSelected.add(index);
    }

    updateSelection(selection, mSelected.contains(index));
    mContext.setOptions(mSelected.size());
  }

  private void updateSelection(View selection, boolean selected) {
    selection.setVisibility(selected ? View.VISIBLE : View.GONE);
    View parent = (View) selection.getParent();
    while (!(parent instanceof MaterialCardView)) parent = (View) parent.getParent();
    MaterialCardView card = (MaterialCardView) parent;
    card.setSelected(selected);
    card.setStrokeColor(mContext.getColor(selected ? R.color.scanner_primary : R.color.scanner_outline));
    card.setStrokeWidth((int) mContext.convertDpToPx(selected ? 2 : 1));
    String description = (String) card.getTag();
    card.setContentDescription(selected ? mContext.getString(R.string.model_selected, description) : description);
  }

  public String getSelected() {
    if (mSelected.isEmpty()) {
      return null;
    } else if (mSelected.size() == 1) {
      return (String)getItem(mSelected.get(0));
    } else {
      return mContext.getString(R.string.more_items) + " (" + mSelected.size() + ")";
    }
  }

  public boolean hasExtension() {
    if (mSelected.isEmpty()) {
      return false;
    }

    return !Exporter.isFolder((String)getItem(mSelected.get(0)));
  }

  public void forwardTouch(MotionEvent event) {
    mGesture.onTouchEvent(event);
  }
}
