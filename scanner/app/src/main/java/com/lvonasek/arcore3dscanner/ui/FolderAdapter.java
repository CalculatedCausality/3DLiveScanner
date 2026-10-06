package com.lvonasek.arcore3dscanner.ui;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.TextView;

import com.lvonasek.arcore3dscanner.R;
import com.lvonasek.arcore3dscanner.main.Exporter;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.function.Consumer;

class FolderAdapter extends BaseAdapter
{
  private final FileManager mContext;
  private final Consumer<String> mListener;
  private File mPath;

  private final ArrayList<String> mItems = new ArrayList<>();

  FolderAdapter(FileManager context, String path, Consumer<String> listener)
  {
    mContext = context;
    mListener = listener;
    mPath = new File(path).getAbsoluteFile();
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
      view = LayoutInflater.from(mContext).inflate(R.layout.view_folder, viewGroup, false);
    }
    String key = (String)getItem(index);
    TextView name = view.findViewById(R.id.name);
    name.setText(key);

    //set icon
    View icon = view.findViewById(R.id.icon);
    icon.setBackground(mContext.getDrawable(R.drawable.ic_folder));
    if (key.compareTo(mContext.getString(R.string.folder_new)) == 0) {
      if (!new File(mPath, key).exists()) {
        icon.setBackground(mContext.getDrawable(R.drawable.ic_folder_new));
      }
    } else if (key.compareTo(mContext.getString(R.string.folder_up)) == 0) {
      icon.setBackground(mContext.getDrawable(R.drawable.ic_folder_up));
    }

    //set open action
    view.setOnClickListener(v -> {
      if (key.compareTo(mContext.getString(R.string.folder_up)) == 0) {
        mPath = mPath.getParentFile();
      } else {
        File folder = new File(mPath, key);
        if (key.compareTo(mContext.getString(R.string.folder_new)) == 0) {
          folder.mkdir();
          mContext.refreshUI();
        }
        mPath = folder;
      }
      mListener.accept(mPath.getAbsolutePath());
      update();
    });

    return view;
  }

  public void update() {
    mItems.clear();
    if (!mPath.equals(new File(AbstractActivity.getPath(false)))) {
      mItems.add(mContext.getString(R.string.folder_up));
    }

    boolean newFolderExist = false;
    String[] files = mPath.list();
    if (files != null) {
      Arrays.sort(files);
      for (String s : files) {
        if (s.compareTo(mContext.getString(R.string.folder_new)) == 0) {
          newFolderExist = true;
        }
        File f = new File(mPath, s);
        if (f.isDirectory() && Exporter.isFolder(s)
                && !f.getAbsolutePath().equals(mContext.getTempPath().getAbsolutePath())) {
          mItems.add(s);
        }
      }
    }

    if (!newFolderExist) {
      mItems.add(mContext.getString(R.string.folder_new));
    }
    notifyDataSetChanged();
  }
}
